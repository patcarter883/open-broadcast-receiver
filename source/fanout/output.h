// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "fanout/frame_ring.h"
#include "fanout/ring.h"
#include "lib/lib.h"

// output — one self-contained fan-out destination (TRANSPORT_PROFILE §3.2):
// its own small GstPipeline, its own bus handling, its own state machine and
// backoff, its own ring cursor feeding its own appsrc. Errors are local by
// construction: a failed output is destroyed and rebuilt, never surgically
// unlinked; nothing here can touch another output or the ingest path.
//
// Reconnect policy (§3.6, FIXPLAN M1.4): every failure retries forever with
// 1 s → ×2 → 30 s-cap backoff — a live event must ride out a platform's
// 5-minute ingest outage without operator action. The only terminal state is
// rtmp_codec_unsupported (a non-H.264 elementary stream cannot enter FLV; it
// cannot self-heal). Auth-class failures are deliberately NOT terminal: many
// RTMP ingests close the TCP connection on a bad stream key, which is
// indistinguishable from a transient reset, so the receiver retries and the
// panel/agent raise "check your stream key" advisories on top of
// reconnects/last_error instead.
class output
{
public:
  enum class run_state : std::uint8_t
  {
    starting,
    running,
    reconnecting,
    error,   // terminal until the next /start
    stopped
  };

  using log_fn = std::function<void(const std::string&)>;

  // cfg must already be validated + pinned (validate_config +
  // validate_and_pin_outputs). The rings must outlive this object. `frames`
  // is the shared decode stage's frame_ring, required for a transcode output
  // and unused (may be nullptr) for a copy-only one.
  output(output_config cfg, ts_ring& ring, frame_ring* frames, log_fn log);
  ~output();
  output(const output&) = delete;
  auto operator=(const output&) -> output& = delete;
  output(output&&) = delete;
  auto operator=(output&&) -> output& = delete;

  // Attach a ring cursor and launch the lifecycle thread. Returns false if no
  // consumer slot is available (cannot happen within the k_max_outputs bound).
  auto start() -> bool;

  // Tear down: pipeline to NULL, detach the cursor, join the thread. Safe to
  // call twice; called by the destructor.
  auto stop() -> void;

  // ---- observability (atomics/snapshots only; /stats reads these) ----

  [[nodiscard]] auto id() const -> const std::string& { return m_cfg.id; }
  [[nodiscard]] auto proto() const -> output_proto { return m_cfg.type; }
  [[nodiscard]] auto transcode() const -> transcode_target
  {
    return m_cfg.transcode.target;
  }
  [[nodiscard]] auto state() const -> run_state
  {
    return m_state.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto state_name() const -> const char*;
  [[nodiscard]] auto reconnects() const -> uint64_t
  {
    return m_reconnects.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto bytes_sent() const -> uint64_t
  {
    return m_bytes_sent.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto dropped_bytes() const -> uint64_t
  {
    return (m_consumer >= 0) ? m_ring.dropped_bytes(m_consumer) : 0;
  }
  // Chosen transcode encoder element name (e.g. "vah264enc"), or "" for a
  // copy-only output / before the first pipeline build.
  [[nodiscard]] auto encoder_name() const -> std::string;
  // Whole-frame drops on this output's frame_ring cursor (transcode only).
  [[nodiscard]] auto frames_dropped() const -> uint64_t
  {
    return (m_frame_consumer >= 0 && m_frames != nullptr)
        ? m_frames->dropped_frames(m_frame_consumer)
        : 0;
  }
  [[nodiscard]] auto audio_dropped() const -> bool
  {
    return m_audio_dropped.load(std::memory_order_relaxed);
  }
  // Seconds in state=running for the current connection, 0 otherwise.
  [[nodiscard]] auto connected_s() const -> int64_t;
  [[nodiscard]] auto last_error() const -> std::string;

  // A new reference to the current attempt's pipeline, or nullptr between
  // attempts (caller owns the reference). The local RTMP harness
  // (test/source/rtmp_publish_harness.cpp) uses it to probe the muxer's pads and
  // read element state: only what the muxer emitted and what the server received
  // show that an output publishes, while bytes_sent counts pushes into the
  // appsrc and can read healthy with nothing leaving the box.
  [[nodiscard]] auto pipeline() const -> GstElement*;

  // Registry pre-check for /start. Returns the first unsatisfied element
  // requirement (base chain => element_unavailable; transcode chain =>
  // transcode_unavailable), or nullopt when every requirement is met. For a
  // transcode output the probe covers the decode -> convert -> encode chain
  // and is satisfied by ANY alternative element name present.
  static auto first_missing_element(output_proto proto,
                                    const transcode_config& transcode,
                                    codec in_codec)
      -> std::optional<missing_requirement>;

private:
  auto worker() -> void;
  // One connect attempt: build → play → feed until error/stop. Returns true
  // if the failure is retryable, false if terminal (state already set).
  auto run_once() -> bool;
  // Transcode feed step (video from frame_ring, AAC audio from ts_ring).
  // Returns false when the attempt should end (appsrc flushing / stopping).
  auto feed_transcode(bool& video_caps_set) -> bool;
  // What a demuxer's src pad carries, decided from its caps at announce time.
  enum class pad_role : std::uint8_t
  {
    h264_video,
    aac_audio,
    other_audio,
    unsupported
  };
  static auto pad_role_of(GstPad* pad) -> pad_role;
  // Link `pad` to `element_name`'s static pad (or requested pad when the name
  // ends in %u). Returns true only when this call made the link.
  auto link_pad_to(GstPad* pad, const char* element_name, const char* pad_name)
      -> bool;
  // Unlink + EOS the muxer's audio pad once, 4 s into an attempt, when it has
  // received no buffer: flvmux emits nothing while that pad is silent and
  // capless, and unlink alone leaves it in the collect aggregate.
  auto probe_muxer_audio() -> void;
  // Set the encoded appsrc caps (raw NV12) from a decoded frame's metadata.
  auto set_video_caps(std::size_t frame_bytes,
                      const frame_ring::frame_meta& meta) -> void;
  // Report the caps arriving at the muxer's video pad, once per attempt:
  // profile, chroma format and bit depth are what a destination refuses first.
  auto log_muxer_input() -> void;
  auto build_pipeline(std::string& err) -> bool;
  auto destroy_pipeline() -> void;
  auto set_state(run_state next) -> void;
  auto set_last_error(const std::string& err) -> void;
  auto log(const std::string& msg) const -> void;

  // Classify a bus error: returns true when the error is the terminal
  // rtmp_codec_unsupported class (caps/negotiation failure on an FLV path).
  auto is_codec_terminal(const GError* err, GstElement* src) const -> bool;

  static auto demux_pad_added(GstElement* demux, GstPad* pad, gpointer self)
      -> void;

  output_config m_cfg;
  ts_ring& m_ring;
  frame_ring* m_frames = nullptr;  // shared decode stage (transcode only)
  log_fn m_log;

  int m_consumer = -1;        // ts_ring cursor (copy video / transcode audio)
  int m_frame_consumer = -1;  // frame_ring cursor (transcode video)
  std::thread m_thread;
  std::atomic_bool m_stopping {false};

  GstElement* m_pipeline = nullptr;
  GstElement* m_appsrc = nullptr;        // video appsrc ("osrc")
  GstElement* m_audio_appsrc = nullptr;  // transcode audio appsrc ("asrc")
  GstBus* m_bus = nullptr;

  std::atomic<run_state> m_state {run_state::starting};
  std::atomic<uint64_t> m_reconnects {0};
  // Set while the transit appsrc is pinned at its limit; drives the stall
  // watchdog in feed_transcode.
  std::chrono::steady_clock::time_point m_stall_since {};
  std::chrono::steady_clock::time_point m_run_started {};
  bool m_audio_probe_done {false};
  bool m_muxer_input_logged {false};
  std::atomic<uint64_t> m_bytes_sent {0};
  std::atomic_bool m_audio_dropped {false};
  // Buffers the muxer's audio pad actually received. Caps on that pad are not
  // data; a linked pad with no buffers is how flvmux stalls without an error.
  std::atomic<uint64_t> m_audio_frames {0};
  std::atomic<int64_t> m_running_since_ms {0};  // steady ms; 0 = not running

  // Raw-frame read buffer for the frame_ring cursor (capacity()-sized; the
  // ring insists a read destination hold a whole frame). Sized once in start().
  std::vector<uint8_t> m_frame_buf;

  mutable std::mutex m_err_mutex;
  std::string m_last_error;

  // Guards the pipeline pointer handed out by pipeline(); every other use of
  // m_pipeline is on the worker thread.
  mutable std::mutex m_pipe_mutex;

  mutable std::mutex m_enc_mutex;
  std::string m_encoder_name;
};

// Build the gst_parse_launch template for an output (TRANSPORT_PROFILE §3.5).
// `encoder` is the concrete transcode encoder element name chosen from the
// shared alternatives list; it is ignored (may be nullptr) for a copy-only
// config. The template carries NO secrets and NO customer URLs: the sink is
// created unconfigured (name=osink) and its connect properties are set by the
// caller, so the string is loggable by definition. Exposed so the full-lane
// test can assert and parse-launch the exact strings used.
auto output_template(output_proto proto,
                     const transcode_config& transcode,
                     const char* encoder) -> std::string;

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H
