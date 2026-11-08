// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_STAGE_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_STAGE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fanout/es_ring.h"
#include "fanout/ring.h"
#include "standby/timeline.h"
#include "standby/stream_switch.h"

#ifdef OBR_HAVE_GSTREAMER
#include <gst/gst.h>
#endif

namespace standby
{

// What the stage needs to know about a session to publish its media.
//
// in_codec/width/height/fps are the session geometry: the parser and the caps
// every branch is pinned to are derived from them, so the live and the slate
// branch cannot drift apart by accident.
//
// slate_video_es/slate_audio_es are the rendered ES files (plan B1). Both empty
// means "no slate at all": no slate branches are built and the stage is a plain
// demux -> ring publisher that still re-stamps.
//
// any_copy_output says that at least one destination copies the stream rather
// than decoding and re-encoding it. Only then does a mid-GOP join to the live
// stream matter, so only then does the video switch wait for a keyframe.
//
// The rings are the session's rings, owned by the caller. video_ring and
// audio_ring are written by the stage (one producer each); input is the TS ring
// the receiver's RIST path fills.
struct stage_config
{
  std::string in_codec;  // h264|h265|av1
  int width = 0;
  int height = 0;
  int fps = 0;
  std::string slate_video_es;
  std::string slate_audio_es;
  bool any_copy_output = false;
  // The mode controller's hysteresis windows. The stage does not decide WHEN to
  // switch, so it does not use them; they ride here so the whole session policy
  // is configured in one place (see max_gap_ms for the one the stage applies).
  std::int64_t live_gap_ms = 0;
  std::int64_t live_hold_ms = 0;
  // The re-stamper's largest forward step passed through unchanged: anything
  // bigger is compressed to one frame so a returning encoder cannot drag the
  // published timeline minutes into the future.
  std::int64_t max_gap_ms = 0;
  es_ring* video_ring = nullptr;
  es_ring* audio_ring = nullptr;
  ts_ring* input = nullptr;
};

struct stage_stats
{
  bool slate_active = false;   // where the VIDEO selector actually is
  std::uint64_t switches = 0;  // set_slate() calls that changed the request
  std::uint64_t rebuilds = 0;
  bool caps_ok = true;      // the two video branches carry identical caps
  // A switch waited longer than its deadline for a keyframe (or for the video
  // switch, for audio). The selector is still on the old branch; the panel
  // should say so rather than report the request as applied.
  bool switch_degraded = false;
  std::uint64_t aus_in = 0;   // AUs handed to a selector's src pad
  std::uint64_t aus_out = 0;  // AUs published into an ES ring
};

#ifdef OBR_HAVE_GSTREAMER

// stage -- the one demux for a session, and the one timeline.
//
// Why one stage rather than a decode chain per destination: the ingest has to be
// demuxed ONCE. Eight outputs each pulling their own tsdemux would multiply the
// demux cost, and worse, each would re-stamp the same stream on its own clock,
// so the muxers would disagree about time. Here a single pipeline does
// TS -> demux -> parse -> select, and publishes COMPRESSED access units into the
// ES rings every output consumes. Everything downstream is a consumer of one
// monotonic timeline.
//
// The pipeline (string-built, secrets-free, logged verbatim):
//
//   appsrc name=dsrc (TS bytes from the session's ts_ring)
//     ! tsparse alignment=7 ! tsdemux name=d
//   [live]  d.<video> ! queue ! <parser> ! <exact caps> ! capsfilter name=vcap0
//   [live]  d.<audio> ! queue ! aacparse ! audio/mpeg,stream-format=raw
//                        ! capsfilter name=acap0
//   [slate] appsrc ! <parser> ! <exact caps> ! capsfilter name=vcap1
//   [slate] appsrc ! aacparse ! audio/mpeg,stream-format=raw
//                        ! capsfilter name=acap1
//   vcap0 -> vsw.sink_0,  vcap1 -> vsw.sink_1,  vsw.src -> pad probe -> fakesink
//   acap0 -> asw.sink_0,  acap1 -> asw.sink_1,  asw.src -> pad probe -> fakesink
//
// tsdemux's pads are dynamic and pad-agnostic `d.` linking would hand the audio
// pad to the video branch (the same reason decoder.cpp links by caps), so the
// demux is left unlinked in the string and routed from pad-added.
//
// The selectors live in their own small bins (vsw/asw) because stream_switch
// finds its selector by the fixed name "sel" and two selectors cannot share a
// name in one bin. One pipeline, one demux, one timeline: the bin is only a
// name-scoping device.
//
// The pad probes are the publishing point and the reason this class exists:
// video's probe calls timeline_state::apply_driver(), audio's calls
// apply_follower(), both on ONE shared instance, and each writes the rewritten
// AU into its ring. Video is the driver because video is what the switch
// decisions are made on; audio follows the offset it chose so lip-sync cannot
// drift at a switch.
//
// rebuild() exists for the reconnection path: a demuxer that has lost its input
// (or a parser that refused a stream) is torn down and rebuilt while the rings
// and the timeline stay alive, so the first AU of the new pipeline continues the
// published timeline instead of restarting it. That is the whole difference
// between a destination that hiccups and one that reconnects.
class stage
{
public:
  using log_fn = std::function<void(const std::string&)>;

  explicit stage(log_fn log = {});
  ~stage();

  stage(const stage&) = delete;
  auto operator=(const stage&) -> stage& = delete;
  stage(stage&&) = delete;
  auto operator=(stage&&) -> stage& = delete;

  // Build the pipeline, attach both switches, start the looper threads and the
  // feed thread, and go to PLAYING. Returns false (nothing left running) when
  // the configuration is unusable or the pipeline cannot be built.
  auto start(const stage_config& cfg) -> bool;

  // Idempotent. Stops the threads, drops the pipeline and releases the ts_ring
  // consumer.
  auto stop() -> void;

  // Ask for the slate (true) or live (false) on BOTH switches. Safe from the
  // mode-controller thread while streaming. A caps mismatch refuses the slate
  // rather than switching into a renegotiation.
  auto set_slate(bool slate) -> void;

  // Rebuild the demux/parse/select chain underneath a running session. The
  // timeline and the rings are NOT reset: the first AU after the rebuild
  // continues the published timeline. Safe while streaming (the feed thread and
  // the loopers are stopped first). Counted in stats().rebuilds.
  auto rebuild() -> bool;

  [[nodiscard]] auto stats() const -> stage_stats;

  // The pipeline string the stage last built, for logs and for the test that
  // asserts the shape. Empty before start().
  [[nodiscard]] auto pipeline_description() const -> std::string;

private:
  // GStreamer handles owned by the stage (every one is a ref we drop).
  struct selector
  {
    GstElement* bin = nullptr;   // the name-scoping bin; owned by the pipeline
    GstElement* sel = nullptr;   // "sel" inside the bin (a ref we hold)
    GstPad* sink0 = nullptr;     // request pads, referenced
    GstPad* sink1 = nullptr;
    GstPad* src = nullptr;
  };

  auto build(std::string& err) -> bool;
  auto attach_switches() -> bool;
  auto add_probes() -> bool;
  auto activate() -> bool;
  auto destroy() -> void;

  auto start_feed() -> void;
  auto stop_feed() -> void;
  auto start_loopers() -> bool;
  auto stop_loopers() -> void;
  auto feed_loop() -> void;

  auto publish(GstPadProbeInfo* info, bool video) -> GstPadProbeReturn;
  auto video_branches_agree() -> bool;
  auto note(const std::string& message) const -> void;

  static auto on_pad_added(GstElement* demux, GstPad* pad, gpointer user)
      -> void;
  auto link_demux_pad(GstPad* pad) -> void;
  static auto video_probe_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user)
      -> GstPadProbeReturn;
  static auto audio_probe_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user)
      -> GstPadProbeReturn;

  log_fn m_log;

  mutable std::mutex m_state_mutex;  // start/stop/rebuild only
  stage_config m_cfg {};
  std::atomic<bool> m_running {false};
  std::string m_description;

  // Kept across rebuild(): this is the timeline continuity guarantee.
  std::unique_ptr<timeline_state> m_timeline;
  std::mutex m_timeline_mutex;  // video (driver) and audio (follower) probes

  es_frame_codec m_codec = es_frame_codec::h264;
  es_nal_framing m_framing = es_nal_framing::annexb;
  std::string m_video_caps;
  std::string m_audio_caps {};

  GstElement* m_pipeline = nullptr;
  GstElement* m_dsrc = nullptr;
  GstElement* m_demux = nullptr;
  GstElement* m_vqueue = nullptr;
  GstElement* m_aqueue = nullptr;
  GstElement* m_vcap0 = nullptr;
  GstElement* m_acap0 = nullptr;
  GstElement* m_vcap1 = nullptr;
  GstElement* m_acap1 = nullptr;
  GstElement* m_vslate_appsrc = nullptr;
  GstElement* m_aslate_appsrc = nullptr;
  GstElement* m_vsink = nullptr;
  GstElement* m_asink = nullptr;
  selector m_vsw;
  selector m_asw;
  guint m_vprobe = 0;
  guint m_aprobe = 0;
  // The pad each probe is installed on: the selector's src when there is a
  // slate, the live capsfilter's src when there is not.
  GstPad* m_vprobe_pad = nullptr;
  GstPad* m_aprobe_pad = nullptr;

  std::unique_ptr<class es_looper> m_vlooper;
  std::unique_ptr<class adts_looper> m_alooper;
  std::atomic<bool> m_slate_ready {false};

  std::shared_ptr<switch_gate> m_gate;
  stream_switch m_vswitch;
  stream_switch m_aswitch;

  int m_consumer = -1;
  std::thread m_feed;
  std::atomic<bool> m_feed_stopping {false};

  std::atomic<bool> m_desired_slate {false};
  std::atomic<bool> m_caps_ok {true};
  std::atomic<std::uint64_t> m_switches {0};
  std::atomic<std::uint64_t> m_rebuilds {0};
  std::atomic<std::uint64_t> m_aus_in {0};
  std::atomic<std::uint64_t> m_aus_out {0};
};

#endif  // OBR_HAVE_GSTREAMER

}  // namespace standby

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_STAGE_H
