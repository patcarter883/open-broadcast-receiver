// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODER_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "fanout/frame_ring.h"
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
// Where each tsdemux src pad goes. BOTH are required: tsdemux returns
// GST_FLOW_NOT_LINKED while it has no linked src pad, and the live ingest exposes
// its audio pad before its video pad -- so a stage that consumes only the video pad
// dies on its first push and never reaches the video pad at all. The non-video pads
// are sunk to remove that race (and because this stage does not decode audio).
struct pad_targets {
  GstElement* video = nullptr;  // transcode branch: queue -> parser -> decoder
  GstElement* other = nullptr;  // everything else, sunk
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
  // thread. Returns false if no ring slot is free or the pipeline cannot be
  // built; the session still runs (copy outputs are independent).
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

private:
  auto worker() -> void;
  auto build_pipeline(std::string& err) -> bool;
  auto destroy_pipeline() -> void;
  auto publish_sample(GstSample* sample) -> void;
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

  GstElement* m_pipeline = nullptr;
  GstElement* m_appsrc = nullptr;  // ref held via gst_bin_get_by_name
  GstElement* m_appsink = nullptr;
  // Kept alive for the lifetime of the pad-added signal connection, which is passed
  // a pointer to this rather than to one element, because both destinations are
  // needed to keep tsdemux flowing.
  pad_targets m_pad_targets;
  GstBus* m_bus = nullptr;
};

// Build the gst_parse_launch template for the decode stage. `source_parser`
// and `decoder` are the concrete element names chosen from the shared
// alternatives lists (lib.h). Exposed so the full-lane test can assert and
// parse-launch the exact string used.
auto decoder_template(codec in_codec,
                      const char* source_parser,
                      const char* decoder) -> std::string;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODER_H
