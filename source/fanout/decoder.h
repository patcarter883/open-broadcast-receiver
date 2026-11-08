// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODER_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "fanout/frame_ring.h"
#include "fanout/pts_synth.h"
#include "fanout/ring.h"
#include "lib/lib.h"

// decoder — the single shared decode stage of the opt-in transcode tier
// (schema_version 3). It is a ts_ring consumer whose pipeline ends in an
// appsink; a feeder thread pulls each decoded buffer and publishes it whole to
// the frame_ring that every transcode output pulls from. Ingest is decoded
// ONCE regardless of how many transcode outputs exist (D-T3).
//
// Constructed ONLY when at least one output requests transcode; a copy-only
// session never builds this stage and pays nothing. Audio is NOT decoded here:
// AAC for an RTMP transcode output is copied from the ts_ring by that output.
//
// A decoder failure is contained: the feeder logs and stops producing, the
// session and copy outputs are unaffected, and transcode outputs simply idle.
// Which decoder: hardware first (the driver is asked to hold on the GPU), with
// the software decoder for the same codec as the last resort. The parser's
// output is pinned to the sink caps the decoder declares, so the stream is
// produced in the layout the hardware decoder takes instead of whatever
// av1parse defaults to. An attempt whose caps cannot be met, that posts an
// error, or that produces no frame inside k_attempt_window is torn down and the
// next entry in the chain is built -- with the reason naming the field the
// decoder refused when the caps are what failed.
// Where each tsdemux src pad goes. Both destinations exist in the bin from
// build_pipeline(): tsdemux returns GST_FLOW_NOT_LINKED while it has no linked
// src pad, and the live ingest exposes its AUDIO pad before its video pad, so
// every non-video pad needs somewhere to go the moment it appears.
//
// `other` is a funnel, not a single sink: it accepts one request pad per
// non-video stream (audio, subtitles, anything else) and the pipeline never has
// to have an element added to it from a streaming thread.
struct pad_targets {
  GstElement* video = nullptr;  // transcode branch: queue -> parser -> decoder
  GstElement* other = nullptr;  // funnel every non-video pad is linked into
  class decoder* owner = nullptr;  // reports a refused video pad's caps
};

class decoder
{
public:
  using log_fn = std::function<void(const std::string&)>;

  // `ring` must outlive this object (owned by main alongside the session).
  decoder(ts_ring& ring, codec in_codec, log_fn log);
  ~decoder();
  decoder(const decoder&) = delete;
  auto operator=(const decoder&) -> decoder& = delete;
  decoder(decoder&&) = delete;
  auto operator=(decoder&&) -> decoder& = delete;

  // Attach a ts_ring cursor, build the decode pipeline and launch the feeder
  // thread. Returns false if no ring slot is free or no attempt could be built;
  // the session still runs (copy outputs are independent).
  auto start() -> bool;

  // Stop the feeder, destroy the pipeline, close the frame_ring (waking every
  // transcode consumer) and detach the cursor. Idempotent.
  auto stop() -> void;

  // The shared hand-off to transcode outputs. Never empty: the object only
  // exists when one is needed.
  [[nodiscard]] auto frames() noexcept -> frame_ring& { return m_frames; }

  [[nodiscard]] auto failed() const noexcept -> bool
  {
    return m_failed.load(std::memory_order_relaxed);
  }

  // One sample of the staged counters and the state the pipeline reached. The
  // periodic log line renders the same data; this accessor exists so a test can
  // assert on it instead of parsing text.
  struct stage_counters {
    std::uint64_t in_chunks = 0;
    std::uint64_t demux_buffers = 0;
    std::uint64_t decoder_buffers = 0;
    std::uint64_t sink_buffers = 0;
    GstState state = GST_STATE_VOID_PENDING;
    bool stalled = false;
    bool failed = false;
  };

  [[nodiscard]] auto counters() const -> stage_counters;
  // The decoder element of the attempt currently (or last) running. Copied
  // under the name lock: the feeder thread replaces it when it degrades.
  [[nodiscard]] auto decoder_element() const -> std::string
  {
    const std::scoped_lock lock {m_name_mutex};
    return m_decoder_name;
  }

  // True while the running attempt is not the hardware-preferred first entry.
  [[nodiscard]] auto using_fallback() const noexcept -> bool
  {
    return m_using_fallback.load(std::memory_order_relaxed);
  }

  // Test hook: retire the first attempt as if its decoder had failed, without
  // waiting for a real driver fault. The rebuild path taken is the same one a
  // NOT_NEGOTIATED or a silent no-frame decoder takes.
  auto simulate_first_attempt_failure() -> void { m_simulate_failure = true; }

  // Entry point of the parser's caps-event probe: records the caps the parser
  // produced (once per attempt) and, when the running decoder's sink template
  // cannot intersect them, the reason -- which the feeder thread turns into the
  // rebuild. Called from a streaming thread.
  auto observe_source_caps(GstCaps* caps) -> void;

private:
  auto worker() -> void;
  auto build_pipeline(std::string& err, const std::string& decoder_name) -> bool;
  auto destroy_pipeline() -> void;
  auto publish_sample(GstSample* sample) -> void;
  // Build, launch and time one attempt from m_chain. On false nothing is left
  // running and the next attempt may be tried.
  auto start_attempt(std::size_t index, std::string& err) -> bool;
  // Retire the running attempt with `reason`, then try every remaining decoder
  // in m_chain in order. Returns false when the chain is exhausted, having
  // marked the stage failed.
  auto degrade(const std::string& reason) -> bool;
  // Bus ERROR / no-frame timeout for the running attempt; promotes to the next
  // decoder when one remains. Returns true when the pipeline was replaced.
  auto check_attempt_health() -> bool;
  // Attach the buffer-count probes: demux output, decoder output, appsink input.
  auto attach_probes(const char* parser_name, const char* decoder_name) -> void;
  // One compact line per k_stats_period with the stage counters, the decoder in
  // use and the state the pipeline actually reached.
  auto log_stats() -> void;
  auto log(const std::string& msg) const -> void;

  ts_ring& m_ring;
  codec m_in_codec;
  log_fn m_log;

  // Owned ring. Sized at the minimum capacity to bound every consumer's
  // read buffer (frame_ring::read requires a capacity()-sized destination);
  // a frame larger than 16 MiB (beyond 4K NV12) is dropped whole by design.
  frame_ring m_frames {frame_ring::k_min_bytes};

  int m_consumer = -1;
  std::thread m_thread;
  std::atomic_bool m_stopping {false};
  std::atomic_bool m_failed {false};
  std::atomic_bool m_using_fallback {false};

  // Decoders the registry has for m_in_codec, hardware first; m_attempt indexes
  // it. The name is kept as a string, not a pointer, because the attempt is torn
  // down and rebuilt between attempts.
  std::vector<std::string> m_chain;
  std::size_t m_attempt = 0;
  std::string m_decoder_name;
  mutable std::mutex m_name_mutex;  // guards m_decoder_name for the accessor
  // Measured from the first TS chunk, not from pipeline build: the stream can
  // start seconds after /start, and a window that expires before any data
  // arrives would call a healthy decoder dead. Both are written by the feed
  // thread and read by the monitor thread.
  std::atomic<std::int64_t> m_attempt_start_ns {0};
  std::atomic<std::uint64_t> m_attempt_base_decoder {0};
  // Whether the stage has ever received a TS chunk. The first wait is longer:
  // a session's stream legitimately starts seconds after /start, whereas a
  // retry that has already seen data and gets nothing is a failed attempt.
  std::atomic<bool> m_ever_fed {false};
  bool m_simulate_failure = false;

  // Decode-stage counters, read only by the periodic line: TS chunks pushed into
  // the demux appsrc, buffers out of tsdemux, buffers out of the decoder, and
  // buffers arriving at the appsink.
  std::atomic<std::uint64_t> m_in_chunks {0};
  std::atomic<std::uint64_t> m_in_bytes {0};
  std::atomic<std::uint64_t> m_demux_buffers {0};
  std::atomic<std::uint64_t> m_decoder_buffers {0};
  std::atomic<std::uint64_t> m_sink_buffers {0};
  // Set while the decode appsrc sits at its byte limit; the feed thread keeps
  // polling rather than blocking, so this only drives the stall report.
  std::atomic_bool m_stalled {false};
  std::chrono::steady_clock::time_point m_stall_since {};

  GstElement* m_pipeline = nullptr;
  GstElement* m_appsrc = nullptr;  // ref held via gst_bin_get_by_name
  GstElement* m_appsink = nullptr;
  // The running decoder's sink template caps, and the caps the parser produced
  // on this attempt. m_caps_refusal is written by the streaming thread and
  // consumed by the feeder, which owns the rebuild.
  GstCaps* m_decoder_caps = nullptr;
  std::atomic_bool m_caps_reported {false};
  std::mutex m_caps_mutex;
  std::string m_caps_refusal;
  // Kept alive for the lifetime of the pad-added signal connection, which is passed
  // a pointer to this rather than to one element, because both destinations are
  // needed to keep tsdemux flowing.
  pad_targets m_pad_targets;
  GstBus* m_bus = nullptr;
  // Monotonic timeline for the frames this stage publishes; survives decoder
  // rebuilds so a re-attempt never rewinds the destination's clock.
  pts_synth m_pts;
};

// Build the gst_parse_launch template for the decode stage. `source_parser`
// and `decoder` are the concrete element names chosen from the shared
// alternatives lists (lib.h). Exposed so the full-lane test can assert and
// parse-launch the exact string used.
auto decoder_template(codec in_codec,
                      const char* source_parser,
                      const char* decoder) -> std::string;

// The caps `decoder` declares on its sink pad, reduced to the fields it fixes
// to a single value (vaav1dec: profile=main, alignment=frame; av1dec:
// alignment=tu). The decode branch pins exactly these between the parser and
// the decoder, so the parser is told which layout to produce instead of
// negotiating its default and letting the decoder refuse it. Empty when the
// element is not in the registry.
auto decoder_branch_caps(const char* decoder) -> std::string;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODER_H
