#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_RESTREAM_RESTREAM_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_RESTREAM_RESTREAM_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "lib/lib.h"

// restream owns the single GStreamer pipeline that receives the demuxed
// MPEG-TS (pushed in via appsrc by the RIST receiver), then for each
// configured destination either copies or reencodes video/audio and muxes to
// RTMP / SRT / RIST. One pipeline, one bus, one teardown — mirroring the
// encoder's encode-class lifecycle discipline. See docs/GSTREAMER.md.
class restream
{
public:
  explicit restream(std::function<void(const std::string&)> log);
  ~restream();
  restream(const restream&) = delete;
  auto operator=(const restream&) -> restream& = delete;
  restream(restream&&) = delete;
  auto operator=(restream&&) -> restream& = delete;

  // Build + launch the pipeline for cfg. `state` receives bus errors and is
  // borrowed for the pipeline's lifetime. On failure returns false and fills
  // err_code (CONTRACT error_code, e.g. "encoder_unavailable",
  // "pipeline_launch_failed") + err_msg.
  auto start(const receiver_config& cfg,
             receiver_state* state,
             std::string& err_code,
             std::string& err_msg) -> bool;

  auto stop() -> void;

  // Push one RIST payload (raw MPEG-TS) into the pipeline. Always returns 0
  // (keep the RIST connection) — transient push failures during teardown are
  // swallowed rather than dropping the peer.
  auto push_buffer(const uint8_t* data, std::size_t len) -> int;

  auto is_running() const noexcept -> bool
  {
    return m_running.load(std::memory_order_acquire);
  }

private:
  auto build_pipeline_string(const receiver_config& cfg,
                             bool redact = false) -> std::string;
  // Returns "" if all required elements are present, else the missing element
  // name (for an encoder_unavailable error).
  auto first_missing_element(const receiver_config& cfg) const -> std::string;

  auto bus_loop() -> void;
  auto clear_pipeline_state() -> void;
  auto log(const std::string& msg) const -> void;

  std::function<void(const std::string&)> m_log_func;

  // Guards pipeline/appsrc/bus and serialises clear_pipeline_state() against
  // push_buffer().
  std::mutex m_pipeline_mutex;
  GstElement* m_pipeline = nullptr;
  GstElement* m_appsrc = nullptr;  // held ref from gst_bin_get_by_name
  GstBus* m_bus = nullptr;

  std::atomic_bool m_running {false};
  std::atomic_bool m_cleaned_up {true};
  std::thread m_bus_thread;

  receiver_state* m_state = nullptr;  // borrowed
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_RESTREAM_RESTREAM_H
