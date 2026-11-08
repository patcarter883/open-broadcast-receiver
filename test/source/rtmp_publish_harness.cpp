// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Local RTMP publish harness.
//
// Drives the receiver's REAL output pipeline (output::start / run_once /
// feed_transcode in source/fanout/output.cpp) against a local nginx-rtmp sink
// and judges it by what the SERVER received. No capture box, no RIST chain, no
// platform: the ingest side is replaced by fixtures this binary generates with
// GStreamer, and the destination is rtmp://127.0.0.1:11935/live/test.
//
// Why the server's counter and not output::bytes_sent(): bytes_sent counts
// pushes into the appsrc, so it reads healthy while nothing leaves the box —
// publish accepted, egress flat, no GStreamer error. The muxer's sink pads are
// probed for the same reason: an appsrc can be caps'd and accepting buffers
// while the branch downstream of it is stalled.
//
// Modes (all use the production templates; nothing here rebuilds a pipeline by
// hand):
//   transcode-video-only  h264 transcode, frame ring plus a video-only
//                         H.264-in-MPEG-TS clip in the ts_ring -- the shape of a
//                         real video-only ingest. The audio branch finds no AAC
//                         pad in that TS, so tsdemux must have its video pad
//                         sunk, and flvmux holds the output until its audio pad
//                         carries data or is unlinked: the code's audio-pad
//                         probe has to drop the pad and video must still reach
//                         the server.
//   transcode-av          h264 transcode, frame ring plus a real AAC-in-MPEG-TS
//                         clip in the ts_ring (the audio copy branch).
//   copy-av               copy-only tier (tsparse/tsdemux/flvmux) fed an
//                         H.264+AAC MPEG-TS clip.
//   copy-video-only       copy-only tier fed a video-only MPEG-TS clip. Expected
//                         to STALL: the copy template links its audio pad by
//                         request pad and has no timeout unlink, so flvmux waits
//                         for audio forever. The harness asserts that
//                         expectation, which is what makes it a test of a known
//                         gap rather than a flaky failure.
//
// Exit status 0 = observed behaviour matched the mode's expectation, 1 = it did
// not (or the harness could not run). The numbers are always printed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <httplib.h>

#include "fanout/decoder.h"
#include "fanout/frame_ring.h"
#include "fanout/output.h"
#include "fanout/ring.h"
#include "lib/lib.h"

// The commit this binary was linked against (test/CMakeLists.txt). A verdict is
// about that commit only: a stale binary has reported a stall the current
// sources do not produce.
#ifndef OBR_SOURCE_COMMIT
#define OBR_SOURCE_COMMIT "unknown"
#endif

namespace
{
// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

enum class mode
{
  transcode_video_only,
  transcode_av,
  transcode_av1,
  copy_av,
  copy_video_only
};

struct options
{
  mode run_mode = mode::transcode_av;
  std::string host = "127.0.0.1";
  int port = 11935;
  std::string app = "live";
  std::string stream = "test";
  std::string stat_url = "http://127.0.0.1:18080/stat";
  std::string fixture;  // AV1-in-MPEG-TS input for the transcode-av1 mode
  int duration_s = 12;
  int fps = 30;
  int width = 640;
  int height = 360;
  int bitrate_kbps = 2000;
  int gop = 30;
};

[[nodiscard]] auto mode_name(mode m) -> const char*
{
  switch (m) {
    case mode::transcode_video_only:
      return "transcode-video-only";
    case mode::transcode_av:
      return "transcode-av";
    case mode::transcode_av1:
      return "transcode-av1";
    case mode::copy_av:
      return "copy-av";
    case mode::copy_video_only:
      return "copy-video-only";
  }
  return "unknown";
}

[[nodiscard]] auto is_copy_mode(mode m) -> bool
{
  return m == mode::copy_av || m == mode::copy_video_only;
}

// What the mode must do for the harness to call the run a pass.
[[nodiscard]] auto expects_publish(mode m) -> bool
{
  return m != mode::copy_video_only;
}

// transcode-av1 is fed an AV1 MPEG-TS that carries no audio, so it takes the
// same video-only path as transcode-video-only (the audio-pad probe must
// retire the capless pad).
[[nodiscard]] auto has_audio(mode m) -> bool
{
  return m == mode::transcode_av || m == mode::copy_av;
}

// Modes whose frame ring is filled by the real decode stage from an AV1
// MPEG-TS instead of by synthetic NV12 frames generated in-process.
[[nodiscard]] auto is_decode_mode(mode m) -> bool
{
  return m == mode::transcode_av1;
}

auto usage(const char* argv0) -> void
{
  std::fprintf(stderr,
               "usage: %s [--mode transcode-av|transcode-video-only|"
               "transcode-av1|copy-av|"
               "copy-video-only]\n"
               "          [--host 127.0.0.1] [--port 11935] [--app live] "
               "[--stream test]\n"
               "          [--stat-url http://127.0.0.1:18080/stat] "
               "[--duration 12] [--fps 30]\n"
               "          [--fixture build/av1-fixture.ts]  (transcode-av1)\n",
               argv0);
}

[[nodiscard]] auto parse_mode(const std::string& text) -> std::optional<mode>
{
  if (text == "transcode-video-only") {
    return mode::transcode_video_only;
  }
  if (text == "transcode-av") {
    return mode::transcode_av;
  }
  if (text == "transcode-av1") {
    return mode::transcode_av1;
  }
  if (text == "copy-av") {
    return mode::copy_av;
  }
  if (text == "copy-video-only") {
    return mode::copy_video_only;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Server-side byte count (nginx-rtmp /stat)
// ---------------------------------------------------------------------------

struct http_target
{
  std::string host;
  int port = 80;
  std::string path = "/";
};

[[nodiscard]] auto parse_http_url(const std::string& url)
    -> std::optional<http_target>
{
  constexpr std::string_view scheme = "http://";
  if (url.rfind(scheme, 0) != 0) {
    return std::nullopt;
  }
  const std::string rest = url.substr(scheme.size());
  const std::size_t slash = rest.find('/');
  const std::string authority = rest.substr(0, slash);
  http_target target;
  target.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string::npos) {
    target.host = authority;
  } else {
    target.host = authority.substr(0, colon);
    target.port = std::atoi(authority.c_str() + colon + 1);
  }
  return target.host.empty() || target.port <= 0
      ? std::nullopt
      : std::optional<http_target> {target};
}

// First <bytes_in> in the document: nginx-rtmp's server-wide counter, which
// survives the publisher disconnecting. Per-stream sections come later and
// disappear with the stream, so they cannot be read after the fact.
[[nodiscard]] auto parse_bytes_in(const std::string& xml)
    -> std::optional<long long>
{
  const std::string open = "<bytes_in>";
  const std::string close = "</bytes_in>";
  const std::size_t a = xml.find(open);
  if (a == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t b = xml.find(close, a);
  if (b == std::string::npos) {
    return std::nullopt;
  }
  const std::string digits = xml.substr(a + open.size(), b - a - open.size());
  try {
    return std::stoll(digits);
  } catch (...) {
    return std::nullopt;
  }
}

struct stat_probe
{
  std::optional<long long> bytes_in;
  std::string error;
  std::string body;
};

// What counts as proof of publication on the server side. nginx-rtmp's bytes_in
// includes the RTMP handshake and the connect/publish commands (~3 KB), so
// "greater than zero" would pass a publisher that connected and then sent
// nothing — the exact shape of the live-rig failure. 32 KiB is ten handshakes
// and a fraction of a second of a 1 Mbit/s stream; with k_min_run it cannot be
// reached without media.
constexpr long long k_server_bytes_proof = 32 * 1024;
constexpr std::chrono::milliseconds k_min_run {2000};

[[nodiscard]] auto fetch_server_bytes(const std::string& url) -> stat_probe
{
  const auto target = parse_http_url(url);
  if (!target) {
    return {std::nullopt, "cannot parse stat url", {}};
  }
  httplib::Client client(target->host, target->port);
  client.set_connection_timeout(2, 0);
  client.set_read_timeout(3, 0);
  const auto response = client.Get(target->path);
  if (!response) {
    return {std::nullopt, "no response from stat endpoint", {}};
  }
  if (response->status != 200) {
    return {std::nullopt,
            std::format("stat endpoint returned {}", response->status),
            response->body};
  }
  const auto bytes = parse_bytes_in(response->body);
  return {bytes, bytes ? std::string {} : "no <bytes_in> in stat document",
          response->body};
}

// ---------------------------------------------------------------------------
// Fixtures: generated locally with GStreamer, never checked in
// ---------------------------------------------------------------------------

// Pull every sample a pipeline produces through its appsink. `on_sample`
// returns false to stop early. Returns false only when the pipeline could not be
// built or played.
auto pull_samples(const std::string& description,
                  const std::function<bool(GstBuffer*, GstCaps*)>& on_sample)
    -> bool
{
  GError* gerr = nullptr;
  GstElement* pipeline = gst_parse_launch(description.c_str(), &gerr);
  if (pipeline == nullptr || gerr != nullptr) {
    std::fprintf(stderr, "fixture pipeline failed to parse: %s\n",
                 gerr != nullptr && gerr->message != nullptr ? gerr->message
                                                             : "unknown");
    if (gerr != nullptr) {
      g_error_free(gerr);
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
    }
    return false;
  }
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  if (sink == nullptr) {
    std::fprintf(stderr, "fixture pipeline has no appsink named sink\n");
    gst_object_unref(pipeline);
    return false;
  }

  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds {30};
  while (ok && std::chrono::steady_clock::now() < deadline) {
    GstSample* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 500 * GST_MSECOND);
    if (sample == nullptr) {
      if (gst_app_sink_is_eos(GST_APP_SINK(sink))) {
        break;
      }
      continue;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    const bool keep_going = on_sample(buffer, caps);
    gst_sample_unref(sample);
    if (!keep_going) {
      break;
    }
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  return ok;
}

// Raw NV12 frames, packed to width*height*3/2 with stride == width, which is the
// layout output::set_video_caps reconstructs width/height from.
struct nv12_clip
{
  int width = 0;
  int height = 0;
  std::vector<std::vector<std::uint8_t>> frames;
};

auto make_nv12_clip(int width, int height, std::size_t count) -> nv12_clip
{
  nv12_clip clip;
  clip.width = width;
  clip.height = height;
  const std::string description = std::format(
      "videotestsrc is-live=false num-buffers={} ! "
      "video/x-raw,format=NV12,width={},height={},framerate=30/1 ! "
      "appsink name=sink sync=false",
      count, width, height);

  pull_samples(description,
               [&clip, width, height, count](GstBuffer* buffer,
                                             GstCaps* caps) -> bool {
                 GstVideoInfo info;
                 if (caps == nullptr || !gst_video_info_from_caps(&info, caps)) {
                   return false;
                 }
                 GstVideoFrame frame;
                 if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
                   return false;
                 }
                 std::vector<std::uint8_t> packed(
                     static_cast<std::size_t>(width)
                     * static_cast<std::size_t>(height) * 3U / 2U);
                 const auto* luma = static_cast<const std::uint8_t*>(
                     GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                 const int luma_stride =
                     GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
                 for (int row = 0; row < height; ++row) {
                   std::memcpy(packed.data()
                                   + (static_cast<std::size_t>(row)
                                      * static_cast<std::size_t>(width)),
                               luma + (static_cast<std::size_t>(row)
                                       * static_cast<std::size_t>(luma_stride)),
                               static_cast<std::size_t>(width));
                 }
                 const auto* chroma = static_cast<const std::uint8_t*>(
                     GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
                 const int chroma_stride =
                     GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
                 const std::size_t luma_bytes =
                     static_cast<std::size_t>(width)
                     * static_cast<std::size_t>(height);
                 for (int row = 0; row < height / 2; ++row) {
                   std::memcpy(packed.data() + luma_bytes
                                   + (static_cast<std::size_t>(row)
                                      * static_cast<std::size_t>(width)),
                               chroma
                                   + (static_cast<std::size_t>(row)
                                      * static_cast<std::size_t>(chroma_stride)),
                               static_cast<std::size_t>(width));
                 }
                 gst_video_frame_unmap(&frame);
                 clip.frames.push_back(std::move(packed));
                 return clip.frames.size() < count;
               });
  return clip;
}

// Concatenated MPEG-TS bytes from a pipeline. mpegtsmux needs no
// enable-custom-mappings here: that flag exists only for AV1, which has no
// standard stream_type; AAC and H.264 do.
auto make_ts(const std::string& description) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes;
  pull_samples(description,
               [&bytes](GstBuffer* buffer, GstCaps*) -> bool {
                 GstMapInfo map;
                 if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                   bytes.insert(bytes.end(), map.data, map.data + map.size);
                   gst_buffer_unmap(buffer, &map);
                 }
                 return true;
               });
  return bytes;
}

[[nodiscard]] auto make_aac_ts(int bitrate_bps, int seconds)
    -> std::vector<std::uint8_t>
{
  // One AAC frame is 1024 samples at 48 kHz; avenc_aac consumes raw PCM frames
  // from audiotestsrc, so the buffer count is the sample budget / 1024.
  const int frames = (48000 / 1024) * seconds;
  return make_ts(std::format(
      "audiotestsrc is-live=false num-buffers={} wave=sine ! "
      "audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate={} ! aacparse ! "
      "mpegtsmux alignment=7 ! appsink name=sink sync=false",
      frames, bitrate_bps));
}

[[nodiscard]] auto make_h264_aac_ts(int width, int height, int bitrate_kbps,
                                    int seconds) -> std::vector<std::uint8_t>
{
  const int frames = (48000 / 1024) * seconds;
  return make_ts(std::format(
      "videotestsrc is-live=false num-buffers={} ! "
      "video/x-raw,format=NV12,width={},height={},framerate=30/1 ! "
      "x264enc tune=zerolatency key-int-max=30 bitrate={} ! "
      "h264parse config-interval=-1 ! mux. "
      "audiotestsrc is-live=false num-buffers={} wave=sine ! "
      "audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=128000 ! "
      "aacparse ! mux. "
      "mpegtsmux name=mux alignment=7 ! appsink name=sink sync=false",
      seconds * 30, width, height, bitrate_kbps, frames));
}

[[nodiscard]] auto make_h264_ts(int width, int height, int bitrate_kbps,
                                int seconds) -> std::vector<std::uint8_t>
{
  return make_ts(std::format(
      "videotestsrc is-live=false num-buffers={} ! "
      "video/x-raw,format=NV12,width={},height={},framerate=30/1 ! "
      "x264enc tune=zerolatency key-int-max=30 bitrate={} ! "
      "h264parse config-interval=-1 ! mpegtsmux alignment=7 ! "
      "appsink name=sink sync=false",
      seconds * 30, width, height, bitrate_kbps));
}

// An AV1-in-MPEG-TS clip produced by test/tools/make-av1-ts-fixture.sh. It is
// read as-is: the harness must not mux its own AV1, because a TS without
// mpegtsmux's enable-custom-mappings carries no AV1 stream type and tsdemux
// exposes no video pad for it.
auto read_ts_file(const std::string& path, std::string& err)
    -> std::vector<std::uint8_t>
{
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    err = std::format("cannot open {}", path);
    return {};
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t chunk[64 * 1024];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
    bytes.insert(bytes.end(), chunk, chunk + got);
  }
  std::fclose(file);
  if (bytes.empty()) {
    err = std::format("{} is empty", path);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Instrumentation on the real pipeline
// ---------------------------------------------------------------------------

struct instruments
{
  std::atomic<std::uint64_t> mux_video_buffers {0};
  std::atomic<std::uint64_t> mux_video_bytes {0};
  std::atomic<std::uint64_t> mux_audio_buffers {0};
  std::atomic<std::uint64_t> mux_audio_bytes {0};
  std::atomic<std::uint64_t> mux_src_buffers {0};
  std::atomic<std::uint64_t> mux_src_bytes {0};
  std::atomic_bool audio_pad_seen {false};

  std::mutex mutex;
  std::string video_caps;
  std::string audio_caps;
  std::vector<std::string> bus_events;
  std::vector<std::string> output_log;

  GstElement* pipeline = nullptr;  // our reference
  GstBus* bus = nullptr;           // ours
};

auto record_caps(instruments& inst, bool audio, GstPad* pad) -> void
{
  {
    std::lock_guard<std::mutex> guard(inst.mutex);
    if ((audio ? inst.audio_caps : inst.video_caps).empty() == false) {
      return;
    }
  }
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps == nullptr) {
    return;
  }
  gchar* text = gst_caps_to_string(caps);
  if (text != nullptr) {
    std::lock_guard<std::mutex> guard(inst.mutex);
    auto& slot = audio ? inst.audio_caps : inst.video_caps;
    if (slot.empty()) {
      slot = text;
    }
    g_free(text);
  }
  gst_caps_unref(caps);
}

auto mux_sink_probe(GstPad* pad, GstPadProbeInfo* info, gpointer data)
    -> GstPadProbeReturn
{
  auto* inst = static_cast<instruments*>(data);
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_PASS;
  }
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  const std::uint64_t size =
      buffer != nullptr ? gst_buffer_get_size(buffer) : 0;
  const char* name = GST_PAD_NAME(pad);
  const bool audio = name != nullptr && g_str_has_prefix(name, "audio") == TRUE;
  if (audio) {
    inst->audio_pad_seen.store(true);
    inst->mux_audio_buffers.fetch_add(1);
    inst->mux_audio_bytes.fetch_add(size);
  } else {
    inst->mux_video_buffers.fetch_add(1);
    inst->mux_video_bytes.fetch_add(size);
  }
  record_caps(*inst, audio, pad);
  return GST_PAD_PROBE_PASS;
}

auto mux_src_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer data)
    -> GstPadProbeReturn
{
  auto* inst = static_cast<instruments*>(data);
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_PASS;
  }
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  inst->mux_src_buffers.fetch_add(1);
  if (buffer != nullptr) {
    inst->mux_src_bytes.fetch_add(gst_buffer_get_size(buffer));
  }
  return GST_PAD_PROBE_PASS;
}

// GST_BUS_PASS keeps output::run_once's own pop_filtered loop working: the
// message is still queued on the bus after it is recorded here.
auto bus_sync_handler(GstBus* /*bus*/, GstMessage* message, gpointer data)
    -> GstBusSyncReply
{
  auto* inst = static_cast<instruments*>(data);
  const GstMessageType type = GST_MESSAGE_TYPE(message);
  if (type != GST_MESSAGE_ERROR && type != GST_MESSAGE_WARNING
      && type != GST_MESSAGE_EOS) {
    return GST_BUS_PASS;
  }
  gchar* source = gst_object_get_name(GST_MESSAGE_SRC(message));
  std::string line = std::format("{} from {}: ",
                                 GST_MESSAGE_TYPE_NAME(message),
                                 source != nullptr ? source : "?");
  g_free(source);
  if (type == GST_MESSAGE_ERROR) {
    GError* gerr = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &gerr, &debug);
    line += (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                          : "unknown";
    if (debug != nullptr) {
      line += std::format(" [debug: {}]", debug);
    }
    if (gerr != nullptr) {
      g_error_free(gerr);
    }
    g_free(debug);
  }
  std::lock_guard<std::mutex> guard(inst->mutex);
  inst->bus_events.push_back(line);
  return GST_BUS_PASS;
}

auto install_instruments(instruments& inst, GstElement* pipeline) -> void
{
  inst.pipeline = GST_ELEMENT(gst_object_ref(pipeline));
  inst.bus = gst_element_get_bus(inst.pipeline);
  if (inst.bus != nullptr) {
    gst_bus_set_sync_handler(inst.bus, &bus_sync_handler, &inst, nullptr);
  }
  GstElement* mux = gst_bin_get_by_name(GST_BIN(inst.pipeline), "mux");
  if (mux == nullptr) {
    return;
  }
  GstIterator* it = gst_element_iterate_sink_pads(mux);
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(it, &item)) {
      case GST_ITERATOR_OK: {
        auto* pad = GST_PAD(g_value_get_object(&item));
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &mux_sink_probe,
                          &inst, nullptr);
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }
  g_value_unset(&item);
  gst_iterator_free(it);

  GstPad* src = gst_element_get_static_pad(mux, "src");
  if (src != nullptr) {
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, &mux_src_probe, &inst,
                      nullptr);
    gst_object_unref(src);
  }
  gst_object_unref(mux);
}

// ---------------------------------------------------------------------------
// Failure diagnostics
// ---------------------------------------------------------------------------

auto dump_element_states(GstElement* pipeline) -> void
{
  std::printf("  element states:\n");
  GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipeline));
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(it, &item)) {
      case GST_ITERATOR_OK: {
        auto* element = GST_ELEMENT(g_value_get_object(&item));
        GstState state = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        gst_element_get_state(element, &state, &pending, 0);
        gchar* name = gst_element_get_name(element);
        std::printf("    %-28s %-16s pending %s\n", name != nullptr ? name : "?",
                    gst_element_state_get_name(state),
                    gst_element_state_get_name(pending));
        g_free(name);
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }
  g_value_unset(&item);
  gst_iterator_free(it);
}

auto element_sink_caps(GstElement* pipeline, const char* name) -> std::string
{
  GstElement* element = gst_bin_get_by_name(GST_BIN(pipeline), name);
  if (element == nullptr) {
    return {};
  }
  std::string text;
  GstPad* pad = gst_element_get_static_pad(element, "sink");
  if (pad != nullptr) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps != nullptr) {
      gchar* rendered = gst_caps_to_string(caps);
      if (rendered != nullptr) {
        text = rendered;
        g_free(rendered);
      }
      gst_caps_unref(caps);
    }
    gst_object_unref(pad);
  }
  gst_object_unref(element);
  return text;
}

auto dump_mux_pads(GstElement* pipeline) -> void
{
  GstElement* mux = gst_bin_get_by_name(GST_BIN(pipeline), "mux");
  if (mux == nullptr) {
    std::printf("  mux: not found in pipeline\n");
    return;
  }
  std::printf("  mux sink pads:\n");
  GstIterator* it = gst_element_iterate_sink_pads(mux);
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(it, &item)) {
      case GST_ITERATOR_OK: {
        auto* pad = GST_PAD(g_value_get_object(&item));
        const bool linked = gst_pad_is_linked(pad) == TRUE;
        GstCaps* caps = gst_pad_get_current_caps(pad);
        gchar* text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
        std::printf("    %-10s linked=%s caps=%s\n", GST_PAD_NAME(pad),
                    linked ? "yes" : "no", text != nullptr ? text : "(none)");
        g_free(text);
        if (caps != nullptr) {
          gst_caps_unref(caps);
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }
  g_value_unset(&item);
  gst_iterator_free(it);

  for (const char* name : {"osrc", "asrc"}) {
    GstElement* element = gst_bin_get_by_name(GST_BIN(pipeline), name);
    if (element == nullptr) {
      continue;
    }
    GstPad* src = gst_element_get_static_pad(element, "src");
    if (src != nullptr) {
      GstCaps* caps = gst_pad_get_current_caps(src);
      gchar* text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
      std::printf("  %s caps=%s linked=%s\n", name,
                  text != nullptr ? text : "(none)",
                  gst_pad_is_linked(src) == TRUE ? "yes" : "no");
      g_free(text);
      if (caps != nullptr) {
        gst_caps_unref(caps);
      }
      gst_object_unref(src);
    }
    gst_object_unref(element);
  }
  gst_object_unref(mux);
}
}  // namespace

auto main(int argc, char** argv) -> int
{
  options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&argc, &argv, &i]() -> std::string {
      return i + 1 < argc ? argv[++i] : std::string {};
    };
    if (arg == "--mode") {
      const auto parsed = parse_mode(next());
      if (!parsed) {
        usage(argv[0]);
        return 1;
      }
      opts.run_mode = *parsed;
    } else if (arg == "--host") {
      opts.host = next();
    } else if (arg == "--port") {
      opts.port = std::atoi(next().c_str());
    } else if (arg == "--app") {
      opts.app = next();
    } else if (arg == "--stream") {
      opts.stream = next();
    } else if (arg == "--stat-url") {
      opts.stat_url = next();
    } else if (arg == "--fixture") {
      opts.fixture = next();
    } else if (arg == "--duration") {
      opts.duration_s = std::atoi(next().c_str());
    } else if (arg == "--fps") {
      opts.fps = std::atoi(next().c_str());
    } else if (arg == "--width") {
      opts.width = std::atoi(next().c_str());
    } else if (arg == "--height") {
      opts.height = std::atoi(next().c_str());
    } else if (arg == "--bitrate") {
      opts.bitrate_kbps = std::atoi(next().c_str());
    } else {
      usage(argv[0]);
      return 1;
    }
  }
  if (opts.fps <= 0 || opts.duration_s <= 0 || opts.width <= 0
      || opts.height <= 0) {
    usage(argv[0]);
    return 1;
  }

  gst_init(nullptr, nullptr);

  const std::string url = std::format("rtmp://{}:{}/{}/{}", opts.host,
                                      opts.port, opts.app, opts.stream);

  // ---- preflight: the same registry check /start does, so a missing element
  // fails here by name instead of as an opaque pipeline build failure. ----
  transcode_config transcode;
  if (!is_copy_mode(opts.run_mode)) {
    transcode.target = transcode_target::h264;
    transcode.bitrate_kbps = opts.bitrate_kbps;
    transcode.gop = opts.gop;
  }
  if (const auto missing =
          output::first_missing_element(output_proto::rtmp, transcode,
                                        codec::h264);
      missing.has_value()) {
    std::fprintf(stderr, "FAIL: missing element: %s\n",
                 missing->element.c_str());
    return 1;
  }

  // ---- server baseline: a sink that is not up makes every later number
  // meaningless, so refuse to publish into it. ----
  stat_probe baseline = fetch_server_bytes(opts.stat_url);
  if (!baseline.bytes_in) {
    std::fprintf(stderr, "FAIL: RTMP sink not reachable at %s (%s)\n",
                 opts.stat_url.c_str(), baseline.error.c_str());
    return 1;
  }

  // ---- fixtures ----
  std::vector<std::uint8_t> ts_clip;
  nv12_clip clip;
  const int fixture_seconds = std::max(opts.duration_s, 6);
  if (is_decode_mode(opts.run_mode)) {
    // Real ingest shape: an AV1-in-MPEG-TS clip read from disk and fed into a
    // real ts_ring, exactly as the RIST callback would. The frames that reach
    // the frame ring are decoded by the receiver's own decode stage.
    if (opts.fixture.empty()) {
      std::fprintf(stderr,
                   "FAIL: mode transcode-av1 needs --fixture <av1.ts> "
                   "(make fixture)\n");
      return 1;
    }
    std::string err;
    ts_clip = read_ts_file(opts.fixture, err);
    if (ts_clip.empty()) {
      std::fprintf(stderr, "FAIL: %s\n", err.c_str());
      return 1;
    }
  } else if (is_copy_mode(opts.run_mode)) {
    ts_clip = has_audio(opts.run_mode)
        ? make_h264_aac_ts(opts.width, opts.height, opts.bitrate_kbps,
                           fixture_seconds)
        : make_h264_ts(opts.width, opts.height, opts.bitrate_kbps,
                       fixture_seconds);
    if (ts_clip.empty()) {
      std::fprintf(stderr, "FAIL: could not generate the MPEG-TS fixture\n");
      return 1;
    }
  } else {
    // One clip cycled with advancing timestamps: a 2 s fixture is enough to
    // keep a live output fed for the whole run without holding hundreds of raw
    // frames in memory.
    clip = make_nv12_clip(opts.width, opts.height, 60);
    if (clip.frames.empty()) {
      std::fprintf(stderr, "FAIL: could not generate NV12 frames\n");
      return 1;
    }
    if (has_audio(opts.run_mode)) {
      ts_clip = make_aac_ts(128000, fixture_seconds);
      if (ts_clip.empty()) {
        std::fprintf(stderr, "FAIL: could not generate the AAC-in-TS fixture\n");
        return 1;
      }
    } else {
      // A video-only ingest still delivers its TS on the ts_ring. The audio
      // branch demuxes it, finds no AAC pad, and must sink the video pad rather
      // than let tsdemux kill the pipeline with "streaming stopped,
      // reason=not-linked".
      ts_clip = make_h264_ts(opts.width, opts.height, opts.bitrate_kbps,
                            fixture_seconds);
      if (ts_clip.empty()) {
        std::fprintf(stderr, "FAIL: could not generate the video-only TS "
                             "fixture\n");
        return 1;
      }
    }
  }

  // ---- the real output, configured the way /start configures it ----
  ts_ring ts_ring_instance {ts_ring::size_for_bandwidth(20000)};
  frame_ring frame_ring_instance {frame_ring::k_min_bytes};
  if (!is_decode_mode(opts.run_mode)
      && frame_ring_instance.capacity()
          < static_cast<std::size_t>(opts.width) * opts.height * 3U / 2U) {
    std::fprintf(stderr, "FAIL: frame ring too small for one frame\n");
    return 1;
  }

  // The decode mode uses the receiver's own decode stage as the frame-ring
  // producer, so the frames that get transcoded are the ones vaav1dec actually
  // produced from the AV1 MPEG-TS -- not frames this harness synthesised.
  std::unique_ptr<decoder> dec;
  if (is_decode_mode(opts.run_mode)) {
    dec = std::make_unique<decoder>(
        ts_ring_instance, codec::av1,
        [](const std::string& line) -> void {
          std::fputs(line.c_str(), stderr);
          std::fflush(stderr);
        });
    if (!dec->start()) {
      std::fprintf(stderr, "FAIL: decode stage refused to start\n");
      return 1;
    }
  }
  frame_ring& frames =
      dec != nullptr ? dec->frames() : frame_ring_instance;

  instruments inst;
  output_config cfg;
  cfg.id = "harness";
  cfg.type = output_proto::rtmp;
  cfg.url = url;
  cfg.host = opts.host;
  cfg.pinned_ip = opts.host;
  cfg.port = opts.port;
  cfg.path = opts.app;
  cfg.key_or_streamid = opts.stream;
  cfg.transcode = transcode;

  output out {cfg, ts_ring_instance,
              is_copy_mode(opts.run_mode) ? nullptr : &frames,
              [&inst](const std::string& line) -> void {
                {
                  std::lock_guard<std::mutex> guard(inst.mutex);
                  inst.output_log.push_back(line);
                }
                std::fputs(line.c_str(), stderr);
              }};

  std::printf("=== RTMP publish harness: mode=%s ===\n", mode_name(opts.run_mode));
  std::printf("build commit   %s\n", OBR_SOURCE_COMMIT);
  std::printf("publish        %s\n", url.c_str());
  std::printf("server stat    %s\n", opts.stat_url.c_str());
  std::printf("server baseline bytes_in %lld\n", *baseline.bytes_in);
  std::fflush(stdout);

  if (!out.start()) {
    std::printf("RESULT         FAIL (output refused to start)\n");
    return 1;
  }

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::seconds {opts.duration_s};

  std::uint64_t frames_pushed = 0;
  std::size_t ts_cursor = 0;
  bool instrumented = false;
  bool server_grew = false;
  std::optional<long long> last_server = baseline.bytes_in;
  auto next_poll = started;

  while (std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration<double>(now - started).count();

    if (!instrumented) {
      if (GstElement* pipeline = out.pipeline(); pipeline != nullptr) {
        install_instruments(inst, pipeline);
        gst_object_unref(pipeline);
        instrumented = true;
      }
    }

    // A copy output takes TS bytes; a transcode output takes whole NV12 frames
    // plus (for the RTMP audio branch) AAC-in-TS on the same ts_ring. The
    // transcode-av1 mode is the exception: its frame ring is filled by the real
    // decode stage, so the harness only feeds TS and never writes a frame.
    if (is_copy_mode(opts.run_mode) || is_decode_mode(opts.run_mode)) {
      const auto want = static_cast<std::size_t>(
          elapsed / static_cast<double>(fixture_seconds)
          * static_cast<double>(ts_clip.size()));
      while (ts_cursor < want && ts_cursor < ts_clip.size()) {
        const std::size_t chunk =
            std::min<std::size_t>(1316, ts_clip.size() - ts_cursor);
        ts_ring_instance.write(ts_clip.data() + ts_cursor, chunk);
        ts_cursor += chunk;
      }
    } else {
      const std::uint64_t target =
          static_cast<std::uint64_t>(elapsed * opts.fps) + 1U;
      while (frames_pushed < target) {
        const auto& frame = clip.frames[frames_pushed % clip.frames.size()];
        frame_ring::frame_meta meta;
        meta.pts = frames_pushed
            * static_cast<std::uint64_t>(1'000'000'000LL / opts.fps);
        meta.dts = meta.pts;
        meta.flags = (frames_pushed % static_cast<std::uint64_t>(opts.fps) == 0)
            ? frame_ring::k_flag_keyframe
            : 0U;
        meta.format = static_cast<std::int32_t>(GST_VIDEO_FORMAT_NV12);
        meta.stride = opts.width;
        if (!frame_ring_instance.write(meta, frame.data(), frame.size())) {
          break;
        }
        ++frames_pushed;
      }
      if (!ts_clip.empty()) {
        const auto want = static_cast<std::size_t>(
            elapsed / static_cast<double>(fixture_seconds)
            * static_cast<double>(ts_clip.size()));
        while (ts_cursor < want && ts_cursor < ts_clip.size()) {
          const std::size_t chunk =
              std::min<std::size_t>(1316, ts_clip.size() - ts_cursor);
          ts_ring_instance.write(ts_clip.data() + ts_cursor, chunk);
          ts_cursor += chunk;
        }
      }
    }

    if (now >= next_poll) {
      next_poll = now + std::chrono::milliseconds {500};
      stat_probe probe = fetch_server_bytes(opts.stat_url);
      if (probe.bytes_in) {
        last_server = probe.bytes_in;
        const long long delta = *probe.bytes_in - *baseline.bytes_in;
        if (now - started >= k_min_run && delta >= k_server_bytes_proof) {
          server_grew = true;
          // The publish is proven; the remaining counters cannot move without
          // it, and the mode checks read them below.
          break;
        }
      }
    }

    // 10 ms ticks: the frame target above is derived from elapsed time, so the
    // feed runs at --fps without a per-frame sleep_until that would busy-spin
    // when the target has already been passed.
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }

  // Settle: the last buffers may still be in flight when the loop exits.
  std::this_thread::sleep_for(std::chrono::milliseconds {400});
  stat_probe final_stat = fetch_server_bytes(opts.stat_url);
  if (final_stat.bytes_in) {
    last_server = final_stat.bytes_in;
    if (*final_stat.bytes_in - *baseline.bytes_in >= k_server_bytes_proof) {
      server_grew = true;
    }
  }

  const bool audio_dropped = out.audio_dropped();
  const std::string last_error = out.last_error();
  const std::string encoder = out.encoder_name();
  const std::uint64_t bytes_sent = out.bytes_sent();
  const int64_t reconnects = static_cast<int64_t>(out.reconnects());
  const char* state_name = out.state_name();
  // Decode-stage provenance, sampled while the stage is still running.
  std::uint64_t decoded_frames = 0;
  decoder::stage_counters decode_counters {};
  std::string decode_element;
  if (dec != nullptr) {
    decoded_frames = dec->frames().head();
    decode_counters = dec->counters();
    decode_element = dec->decoder_element();
  }

  // The publish verdict is the server's counter; the mode checks below are
  // additional assertions about behaviour that "bytes arrived" alone would not
  // pin down. Diagnostics must run while the attempt's pipeline still exists.
  const auto mux_audio_final = inst.mux_audio_buffers.load();
  bool ok = server_grew == expects_publish(opts.run_mode);
  std::string mode_failure;
  if (ok && server_grew && has_audio(opts.run_mode) && mux_audio_final == 0) {
    ok = false;
    mode_failure = "audio never reached the muxer";
  }
  if (ok && server_grew && !has_audio(opts.run_mode) && mux_audio_final != 0) {
    ok = false;
    mode_failure = "audio reached the muxer in a video-only mode";
  }
  // The decode mode exists to prove the AV1 -> NV12 -> H.264 -> RTMP chain, so
  // publishing without the decode stage having produced frames would be a pass
  // for the wrong reason.
  if (ok && is_decode_mode(opts.run_mode)) {
    if (decoded_frames == 0) {
      ok = false;
      mode_failure = "decode stage published no frame to the frame ring";
    } else if (decode_counters.failed || decode_counters.stalled) {
      ok = false;
      mode_failure = "decode stage reported failed/stalled";
    }
  }

  // The destinations are 4:2:0 8-bit, so that is what has to reach the muxer. An
  // encoder whose input is left unpinned negotiates whatever its widest sink
  // caps allow (4:4:4, or a 10-bit layout) and the muxer then publishes a stream
  // the destination cannot decode -- and in the FLV case, one it cannot even
  // mux. Assert on what actually negotiated rather than on the template.
  std::string encoder_input_caps;
  {
    GstElement* pipeline = out.pipeline();
    if (pipeline != nullptr) {
      if (opts.run_mode == mode::transcode_av
          || opts.run_mode == mode::transcode_video_only
          || opts.run_mode == mode::transcode_av1) {
        encoder_input_caps = element_sink_caps(pipeline, "venc");
      }
      if (!ok) {
        dump_element_states(pipeline);
        dump_mux_pads(pipeline);
      }
      gst_object_unref(pipeline);
    } else if (!ok) {
      std::printf("  pipeline: none (attempt already torn down)\n");
    }
  }

  {
    const auto mux_video_final = inst.mux_video_buffers.load();
    std::lock_guard<std::mutex> guard(inst.mutex);
    // Only a mode that actually published is judged: copy-video-only is pinned
    // as an expected stall, and its caps say nothing when nothing was muxed.
    if (ok && server_grew && mux_video_final != 0) {
      const bool is_420 = inst.video_caps.find("chroma-format=(string)4:2:0")
          != std::string::npos;
      const bool is_8bit =
          inst.video_caps.find("bit-depth-luma=(uint)8") != std::string::npos;
      if (!is_420 || !is_8bit) {
        ok = false;
        mode_failure = "muxer video input is not 4:2:0 8-bit";
      }
    }
  }
  if (ok && !encoder_input_caps.empty()
      && encoder_input_caps.find("format=(string)NV12") == std::string::npos) {
    ok = false;
    mode_failure = "encoder input is not NV12 (4:2:0, 8-bit)";
  }

  const bool failed = !ok;

  // Copied out before stop(): the output's log callback takes this same mutex
  // from the worker thread that stop() joins.
  std::string video_caps;
  std::string audio_caps;
  std::vector<std::string> bus_events;
  std::vector<std::string> output_log;
  {
    std::lock_guard<std::mutex> guard(inst.mutex);
    video_caps = inst.video_caps;
    audio_caps = inst.audio_caps;
    bus_events = inst.bus_events;
    output_log = inst.output_log;
  }

  out.stop();
  std::vector<std::string> decode_log;
  if (dec != nullptr) {
    decode_log.push_back(std::format(
        "decode element {} demux={} dec={} sink={} state={} stalled={}",
        decode_element.empty() ? "(none)" : decode_element,
        decode_counters.demux_buffers, decode_counters.decoder_buffers,
        decode_counters.sink_buffers,
        gst_element_state_get_name(decode_counters.state),
        decode_counters.stalled ? "yes" : "no"));
    dec->stop();
    dec.reset();
  }

  const long long server_delta =
      (last_server ? *last_server : 0) - *baseline.bytes_in;

  std::printf("\n");
  std::printf("encoder              %s\n",
              encoder.empty() ? "(copy-only)" : encoder.c_str());
  std::printf("output state         %s (reconnects %lld)\n", state_name,
              static_cast<long long>(reconnects));
  std::printf("frames pushed        %llu\n",
              static_cast<unsigned long long>(frames_pushed));
  if (is_decode_mode(opts.run_mode)) {
    std::printf("decode frames        %llu (ring head, from %s)\n",
                static_cast<unsigned long long>(decoded_frames),
                decode_element.empty() ? "(none)" : decode_element.c_str());
    std::printf("decode counters      demux=%llu dec=%llu sink=%llu "
                "state=%s stalled=%s failed=%s\n",
                static_cast<unsigned long long>(decode_counters.demux_buffers),
                static_cast<unsigned long long>(
                    decode_counters.decoder_buffers),
                static_cast<unsigned long long>(decode_counters.sink_buffers),
                gst_element_state_get_name(decode_counters.state),
                decode_counters.stalled ? "yes" : "no",
                decode_counters.failed ? "yes" : "no");
  }
  std::printf("mux sink video       %llu buffers / %llu bytes\n",
              static_cast<unsigned long long>(
                  inst.mux_video_buffers.load()),
              static_cast<unsigned long long>(inst.mux_video_bytes.load()));
  std::printf("mux sink audio       %llu buffers / %llu bytes\n",
              static_cast<unsigned long long>(
                  inst.mux_audio_buffers.load()),
              static_cast<unsigned long long>(inst.mux_audio_bytes.load()));
  std::printf("mux src emitted      %llu buffers / %llu bytes\n",
              static_cast<unsigned long long>(inst.mux_src_buffers.load()),
              static_cast<unsigned long long>(inst.mux_src_bytes.load()));
  std::printf("server bytes_in      %lld (baseline %lld, delta %lld)\n",
              last_server ? *last_server : -1LL, *baseline.bytes_in,
              server_delta);
  std::printf("output bytes_sent    %llu  (appsrc pushes, not evidence)\n",
              static_cast<unsigned long long>(bytes_sent));
  std::printf("audio pad            dropped=%s caps=%s\n",
              audio_dropped ? "yes" : "no",
              audio_caps.empty() ? "(none)" : audio_caps.c_str());
  std::printf("video caps           %s\n",
              video_caps.empty() ? "(none)" : video_caps.c_str());
  if (!encoder_input_caps.empty()) {
    std::printf("encoder input caps   %s\n", encoder_input_caps.c_str());
  }
  if (!last_error.empty()) {
    std::printf("last error           %s\n", last_error.c_str());
  }
  if (bus_events.empty()) {
    std::printf("bus                  (no error/warning/EOS)\n");
  } else {
    for (const auto& event : bus_events) {
      std::printf("bus                  %s\n", event.c_str());
    }
  }
  if (failed && !output_log.empty()) {
    std::printf("output log:\n");
    for (const auto& line : output_log) {
      std::printf("  %s", line.c_str());
    }
  }

  // Behaviour the mode exists to pin down, beyond "the server got bytes".
  for (const auto& line : decode_log) {
    std::printf("decode stage         %s\n", line.c_str());
  }
  if (opts.run_mode == mode::transcode_video_only && server_grew
      && audio_dropped) {
    std::printf("audio unlink         the audio-pad probe dropped the capless pad "
                "and video still published\n");
  }

  std::printf("RESULT               %s\n", failed ? "FAIL" : "PASS");
  if (!mode_failure.empty()) {
    std::printf("check failed         %s\n", mode_failure.c_str());
  }
  if (failed && !expects_publish(opts.run_mode)) {
    std::printf("expected             mode %s must NOT publish; it did\n",
                mode_name(opts.run_mode));
  }
  if (failed && expects_publish(opts.run_mode)) {
    std::printf("expected             >= %lld new bytes at %s (handshake is "
                "not proof); server delta %lld\n",
                k_server_bytes_proof, opts.stat_url.c_str(), server_delta);
  }
  return failed ? 1 : 0;
}
