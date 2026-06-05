#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_RESTREAM_RESTREAM_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_RESTREAM_RESTREAM_H

#include <atomic>
#include <chrono>
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
// MPEG-TS (pushed in via appsrc by the RIST receiver), DECODES video + audio,
// and hands the uncompressed result to the local restreaming package
// (datarhei/restreamer): raw video frames to a v4l2loopback device and PCM
// audio to an ALSA snd-aloop device. One pipeline, one bus, one teardown.
// See docs/GSTREAMER.md.
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
  // Build the decode->raw-sink pipeline string for the *detected* input codecs
  // (video over RIST + the demuxed audio). No secrets to redact (device paths).
  auto build_pipeline_string(const receiver_config& cfg,
                             codec in_video,
                             audio_codec in_audio) -> std::string;
  // Returns "" if all elements required for the detected codecs are present,
  // else the missing element name (for an encoder_unavailable error).
  auto first_missing_element(const receiver_config& cfg,
                             codec in_video,
                             audio_codec in_audio) const -> std::string;
  // Subset of first_missing_element that checks only the output-side elements
  // (independent of the input codec) — used for the synchronous /start check
  // before detection has run.
  auto first_missing_output_element(const receiver_config& cfg) const
      -> std::string;

  // Phase 1: probe the live MPEG-TS to learn the real codecs before committing
  // to a pipeline. The detection pipeline (appsrc -> tsparse -> tsdemux) is fed
  // by push_buffer just like the real one; tsdemux pad caps reveal the codecs.
  auto start_detection(std::string& err_code, std::string& err_msg) -> bool;
  auto on_demux_pad_added(GstPad* pad) -> void;
  // Classify a demuxed pad's caps into a video/audio codec and record it.
  auto record_caps(const GstCaps* caps) -> void;
  // Phase 2: tear down detection and launch the real pipeline for the codecs we
  // found (falling back to the declared source codec / AAC if undetected).
  auto finish_detection_and_launch() -> void;

  auto bus_loop() -> void;
  auto clear_pipeline_state() -> void;
  auto log(const std::string& msg) const -> void;

  // GSignal/probe trampolines (static so they have access to private members).
  static auto demux_pad_added_trampoline(GstElement* demux,
                                         GstPad* pad,
                                         gpointer user_data) -> void;
  static auto demux_no_more_pads_trampoline(GstElement* demux,
                                            gpointer user_data) -> void;
  static auto caps_event_probe(GstPad* pad,
                               GstPadProbeInfo* info,
                               gpointer user_data) -> GstPadProbeReturn;

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

  // Codec detection state (phase 1). Guarded by m_detect_mutex; written from
  // GStreamer streaming threads (pad-added), read by the bus/worker thread.
  enum class phase : std::uint8_t
  {
    detecting,
    running
  };
  std::atomic<phase> m_phase {phase::detecting};
  std::mutex m_detect_mutex;
  bool m_video_found = false;
  codec m_in_video = codec::h264;
  bool m_audio_found = false;
  audio_codec m_in_audio = audio_codec::aac;
  bool m_no_more_pads = false;
  std::chrono::steady_clock::time_point m_detect_started;

  receiver_config m_cfg;             // captured config for phase-2 launch
  receiver_state* m_state = nullptr;  // borrowed
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_RESTREAM_RESTREAM_H
