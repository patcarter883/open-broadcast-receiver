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

#include <gst/gst.h>

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
  // validate_and_pin_outputs). The ring reference must outlive this object.
  output(output_config cfg, ts_ring& ring, log_fn log);
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
  [[nodiscard]] auto audio_dropped() const -> bool
  {
    return m_audio_dropped.load(std::memory_order_relaxed);
  }
  // Seconds in state=running for the current connection, 0 otherwise.
  [[nodiscard]] auto connected_s() const -> int64_t;
  [[nodiscard]] auto last_error() const -> std::string;

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
  log_fn m_log;

  int m_consumer = -1;
  std::thread m_thread;
  std::atomic_bool m_stopping {false};

  GstElement* m_pipeline = nullptr;
  GstElement* m_appsrc = nullptr;  // ref held via gst_bin_get_by_name
  GstBus* m_bus = nullptr;

  std::atomic<run_state> m_state {run_state::starting};
  std::atomic<uint64_t> m_reconnects {0};
  std::atomic<uint64_t> m_bytes_sent {0};
  std::atomic_bool m_audio_dropped {false};
  std::atomic<int64_t> m_running_since_ms {0};  // steady ms; 0 = not running

  mutable std::mutex m_err_mutex;
  std::string m_last_error;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_OUTPUT_H
