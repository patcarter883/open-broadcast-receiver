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
// Why the server's counter and not the receiver's publish counters: the sink pad
// proves buffers left the pipeline, but only the server proves they arrived.
// output::bytes_published_total() measures the same pad, so the two should agree.
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
//                         H.264+AAC MPEG-TS clip whose PMT lists the video
//                         stream first.
//   copy-audio-first      the same tier fed a clip whose PMT lists the audio
//                         stream first, so tsdemux announces the audio pad
//                         before the video pad. The demuxer's pads are linked by
//                         caps identity, so arrival order cannot decide which
//                         parser a pad reaches.
//   copy-non-aac          copy-only tier fed H.264 with AC-3 audio: an audio pad
//                         no branch can take, so it must be linked to the drop
//                         funnel instead of being left unlinked (which fails
//                         tsdemux with "streaming stopped, reason=not-linked").
//   copy-video-only       copy-only tier fed a video-only MPEG-TS clip. No audio
//                         pad is announced, so the muxer's audio pad is never
//                         requested and video alone publishes.
//   reconnect             transcode-video-only plus a deliberate publisher drop.
//                         Once the publish and the measured-link ramp are proven,
//                         the sink is restarted so the publish is severed and the
//                         destination is briefly unreachable. The output must
//                         rebuild at the connect bootstrap rate (not the measured
//                         target), republish within the connect window, and ramp
//                         back to the measured target once the new publish is
//                         confirmed.
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
#include <set>
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
  copy_audio_first,
  copy_non_aac,
  copy_video_only,
  copy_es,
  reconnect
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
  // reconnect mode: the link measurement the controller reports once the publish
  // is established (the encoder must be at the derived target before the drop),
  // and the command that drops the publisher (e.g. `docker restart <sink>`).
  int measured_kbps = 6000;
  std::string sink_restart_cmd;
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
    case mode::copy_audio_first:
      return "copy-audio-first";
    case mode::copy_non_aac:
      return "copy-non-aac";
    case mode::copy_video_only:
      return "copy-video-only";
    case mode::copy_es:
      return "copy-es";
    case mode::reconnect:
      return "reconnect";
  }
  return "unknown";
}

[[nodiscard]] auto is_copy_mode(mode m) -> bool
{
  return m == mode::copy_av || m == mode::copy_audio_first
      || m == mode::copy_non_aac || m == mode::copy_video_only
      || m == mode::copy_es;
}

// copy-es is a copy output fed from the standby stage's es_rings: compressed
// access units instead of a transport stream, so it has no TS fixture and no
// demux pad order to measure.
[[nodiscard]] auto is_es_mode(mode m) -> bool
{
  return m == mode::copy_es;
}

// transcode-av1 is fed an AV1 MPEG-TS that carries no audio, so it takes the
// same video-only path as transcode-video-only: no audio pad is announced and
// none is requested on the muxer.
[[nodiscard]] auto has_audio(mode m) -> bool
{
  return m == mode::transcode_av || m == mode::copy_av
      || m == mode::copy_audio_first || m == mode::copy_es;
}

// The copy template dispatches tsdemux's pads by caps, so pad order cannot
// decide which parser a pad reaches; this is the mode that feeds the order in
// which the audio pad arrives FIRST, and the verdict requires the live pipeline
// to have seen that order.
[[nodiscard]] auto wants_audio_first(mode m) -> bool
{
  return m == mode::copy_audio_first;
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
               "copy-audio-first|copy-non-aac|copy-video-only|copy-es|"
               "reconnect]\n"
               "          [--host 127.0.0.1] [--port 11935] [--app live] "
               "[--stream test]\n"
               "          [--stat-url http://127.0.0.1:18080/stat] "
               "[--duration 12] [--fps 30]\n"
               "          [--fixture build/av1-fixture.ts]  (transcode-av1)\n"
               "          [--measured-kbps 6000 --sink-restart-cmd 'docker "
               "restart <sink>']  (reconnect)\n",
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
  if (text == "copy-audio-first") {
    return mode::copy_audio_first;
  }
  if (text == "copy-non-aac") {
    return mode::copy_non_aac;
  }
  if (text == "copy-video-only") {
    return mode::copy_video_only;
  }
  if (text == "copy-es") {
    return mode::copy_es;
  }
  if (text == "reconnect") {
    return mode::reconnect;
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

// mpegtsmux numbers its streams in request-pad order, so the order the two
// branches are written decides the PMT's stream order — and tsdemux announces
// its src pads in PMT order. `audio_first` therefore produces a TS whose audio
// pad exists before its video pad, which is the ordering the copy template must
// not depend on.
[[nodiscard]] auto make_h264_aac_ts(int width, int height, int bitrate_kbps,
                                    int seconds, bool audio_first)
    -> std::vector<std::uint8_t>
{
  const int frames = (48000 / 1024) * seconds;
  const std::string video = std::format(
      "videotestsrc is-live=false num-buffers={} ! "
      "video/x-raw,format=NV12,width={},height={},framerate=30/1 ! "
      "x264enc tune=zerolatency key-int-max=30 bitrate={} ! "
      "h264parse config-interval=-1 ! mux. ",
      seconds * 30, width, height, bitrate_kbps);
  const std::string audio = std::format(
      "audiotestsrc is-live=false num-buffers={} wave=sine ! "
      "audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=128000 ! "
      "aacparse ! mux. ", frames);
  return make_ts(std::format("{}{}mpegtsmux name=mux alignment=7 ! "
                             "appsink name=sink sync=false",
                             audio_first ? audio : video,
                             audio_first ? video : audio));
}

// H.264 video with AC-3 audio: an audio ES no RTMP branch can carry, so the
// demuxer's audio pad has to be sunk rather than left unlinked.
[[nodiscard]] auto make_h264_ac3_ts(int width, int height, int bitrate_kbps,
                                    int seconds) -> std::vector<std::uint8_t>
{
  const int frames = (48000 / 1024) * seconds;
  return make_ts(std::format(
      "videotestsrc is-live=false num-buffers={} ! "
      "video/x-raw,format=NV12,width={},height={},framerate=30/1 ! "
      "x264enc tune=zerolatency key-int-max=30 bitrate={} ! "
      "h264parse config-interval=-1 ! mux. "
      "audiotestsrc is-live=false num-buffers={} wave=sine ! "
      "audio/x-raw,rate=48000,channels=2 ! avenc_ac3 bitrate=128000 ! mux. "
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

// The order tsdemux announces its src pads for a TS clip. Pad order is the
// property the copy template must not depend on, so the harness measures it and
// names it in the report instead of trusting the fixture's intent.

// ---------------------------------------------------------------------------
// ES fixture (copy-es mode): compressed access units, not a transport stream.
// The standby stage publishes exactly this shape — one whole AU per ring entry
// with its own PTS/DTS and keyframe flag — so the fixture is generated the same
// way the real stage would: encode, parse to AU alignment, and keep each buffer
// as one AU. Generated at run time with the same elements the other modes use,
// so there is no binary fixture to keep in sync with the encoders.
// ---------------------------------------------------------------------------
struct es_au
{
  std::vector<std::uint8_t> bytes;
  std::int64_t pts_ns = 0;
  bool keyframe = false;
};

struct es_clip
{
  std::vector<es_au> video;
  std::vector<es_au> audio;
  std::int64_t duration_ns = 0;       // one pass through the clip (video)
  std::int64_t audio_duration_ns = 0; // one pass through the clip (audio)
};

auto make_es_clip(int width, int height, int bitrate_kbps, int fps, int seconds)
    -> es_clip
{
  es_clip clip;
  pull_samples(
      std::format(
          "videotestsrc is-live=false num-buffers={} ! "
          "video/x-raw,format=I420,width={},height={},framerate={}/1 ! "
          "x264enc tune=zerolatency bitrate={} key-int-max={} ! "
          "h264parse config-interval=-1 ! "
          "video/x-h264,stream-format=byte-stream,alignment=au ! "
          "appsink name=sink",
          seconds * fps, width, height, fps, bitrate_kbps, fps),
      [&clip](GstBuffer* buffer, GstCaps*) -> bool
      {
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
          return true;
        }
        es_au au;
        au.bytes.assign(map.data, map.data + map.size);
        au.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer)
            ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer))
            : (clip.video.empty()
                   ? 0
                   : clip.video.back().pts_ns + 1'000'000'000LL / 30);
        // An absent DELTA_UNIT flag is a keyframe (IDR) — the same rule the
        // decode stage applies to decoded buffers.
        au.keyframe =
            !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        gst_buffer_unmap(buffer, &map);
        clip.video.push_back(std::move(au));
        return true;
      });

  // ADTS, because that is what a demuxer hands a muxer for AAC and what the
  // stage publishes: self-describing, so au_meta carries no codec_data.
  pull_samples(
      std::format(
          "audiotestsrc is-live=false num-buffers={} ! "
          "audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=128000 ! "
          "aacparse ! audio/mpeg,stream-format=adts ! appsink name=sink",
          seconds * 47),
      [&clip](GstBuffer* buffer, GstCaps*) -> bool
      {
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
          return true;
        }
        es_au au;
        au.bytes.assign(map.data, map.data + map.size);
        au.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer)
            ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer))
            : (clip.audio.empty()
                   ? 0
                   : clip.audio.back().pts_ns + 1'000'000'000LL / 47);
        gst_buffer_unmap(buffer, &map);
        clip.audio.push_back(std::move(au));
        return true;
      });

  // Normalise to a zero origin so the feed's clock is the run's clock.
  if (!clip.video.empty()) {
    const std::int64_t base = clip.video.front().pts_ns;
    for (es_au& au : clip.video) {
      au.pts_ns -= base;
    }
    clip.duration_ns =
        clip.video.back().pts_ns + 1'000'000'000LL / std::max(fps, 1);
  }
  if (!clip.audio.empty()) {
    const std::int64_t base = clip.audio.front().pts_ns;
    for (es_au& au : clip.audio) {
      au.pts_ns -= base;
    }
    clip.audio_duration_ns =
        clip.audio.back().pts_ns + 1'000'000'000LL * 1024 / 48000;
  }
  return clip;
}

// The order tsdemux announces its src pads for a TS clip. Pad order is the
// property the copy template must not depend on, so the harness measures it and
// names it in the report instead of trusting the fixture's intent.
auto probe_demux_pad_order(const std::vector<std::uint8_t>& ts)
    -> std::vector<std::string>
{
  struct collector
  {
    std::mutex mutex;
    std::vector<std::string> pads;
  } seen;

  GError* gerr = nullptr;
  GstElement* pipeline = gst_parse_launch(
      "appsrc name=src is-live=false format=time block=true "
      "caps=video/mpegts,systemstream=true ! tsparse set-timestamps=true "
      "alignment=7 ! tsdemux name=d d. ! fakesink sync=false async=false",
      &gerr);
  if (pipeline == nullptr || gerr != nullptr) {
    std::fprintf(stderr, "fixture pad-order pipeline failed to parse: %s\n",
                 gerr != nullptr && gerr->message != nullptr ? gerr->message
                                                             : "unknown");
    if (gerr != nullptr) {
      g_error_free(gerr);
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
    }
    return {};
  }
  GstElement* demux = gst_bin_get_by_name(GST_BIN(pipeline), "d");
  GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
  if (demux != nullptr) {
    g_signal_connect_data(
        demux, "pad-added",
        G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer data) -> void {
          auto* into = static_cast<collector*>(data);
          std::lock_guard<std::mutex> guard(into->mutex);
          into->pads.emplace_back(GST_PAD_NAME(pad));
        }),
        &seen, nullptr, GConnectFlags(0));
  }
  if (demux == nullptr || src == nullptr
      || gst_element_set_state(pipeline, GST_STATE_PLAYING)
          == GST_STATE_CHANGE_FAILURE)
  {
    if (demux != nullptr) {
      gst_object_unref(demux);
    }
    if (src != nullptr) {
      gst_object_unref(src);
    }
    gst_object_unref(pipeline);
    return {};
  }
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, ts.size(), nullptr);
  GstMapInfo map;
  if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    std::memcpy(map.data, ts.data(), ts.size());
    gst_buffer_unmap(buffer, &map);
    gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
  }
  gst_app_src_end_of_stream(GST_APP_SRC(src));
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_timed_pop_filtered(
      bus, 5 * GST_SECOND,
      (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  if (msg != nullptr) {
    gst_message_unref(msg);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  std::vector<std::string> order;
  {
    std::lock_guard<std::mutex> guard(seen.mutex);
    order = seen.pads;
  }
  if (demux != nullptr) {
    gst_object_unref(demux);
  }
  if (src != nullptr) {
    gst_object_unref(src);
  }
  if (bus != nullptr) {
    gst_object_unref(bus);
  }
  gst_object_unref(pipeline);
  return order;
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
  // Order in which tsdemux exposed its src pads, with each pad's caps name. This
  // is what says whether a run actually exercised the audio-first order.
  std::vector<std::string> demux_pads;
  // Mux sink pads already carrying a counting probe. The muxer's audio pad is
  // requested only when the source TS exposes an audio pad, so it can appear
  // after the pipeline started and has to be picked up then.
  std::set<std::string> probed_mux_pads;
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

// Records the order in which the demuxer exposes its src pads. Pad selection is
// by caps, so the order must not change the verdict -- printing it (and
// requiring it in the audio-first mode) is what keeps that a measured statement
// instead of an assumption about which order a run exercised.
auto demux_pad_added_order(GstElement* /*demux*/, GstPad* pad, gpointer data)
    -> void
{
  auto* inst = static_cast<instruments*>(data);
  std::string entry = GST_PAD_NAME(pad) != nullptr ? GST_PAD_NAME(pad) : "?";
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps != nullptr) {
    if (gst_caps_get_size(caps) > 0) {
      entry += std::format(
          " ({})", gst_structure_get_name(gst_caps_get_structure(caps, 0)));
    }
    gst_caps_unref(caps);
  }
  std::lock_guard<std::mutex> guard(inst->mutex);
  inst->demux_pads.push_back(entry);
}

// Count what each of the muxer's sink pads receives, once per pad. The audio pad
// is a request pad the output asks for when the source TS carries audio, so a
// pad that appears mid-run is picked up by the next call.
auto attach_mux_sink_probes(instruments& inst) -> void
{
  if (inst.pipeline == nullptr) {
    return;
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
        const std::string pad_name = GST_PAD_NAME(pad) != nullptr
            ? GST_PAD_NAME(pad)
            : "?";
        bool fresh = false;
        {
          std::lock_guard<std::mutex> guard(inst.mutex);
          fresh = inst.probed_mux_pads.insert(pad_name).second;
        }
        if (fresh) {
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &mux_sink_probe,
                            &inst, nullptr);
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
  gst_object_unref(mux);
}

auto install_instruments(instruments& inst, GstElement* pipeline) -> void
{
  inst.pipeline = GST_ELEMENT(gst_object_ref(pipeline));
  inst.bus = gst_element_get_bus(inst.pipeline);
  if (inst.bus != nullptr) {
    gst_bus_set_sync_handler(inst.bus, &bus_sync_handler, &inst, nullptr);
  }
  // The demuxer's pad order is part of the verdict for the audio-first mode.
  // Connect before any pad exists and record any that arrived first, so an
  // install that loses a race with the first TS bytes still sees the order.
  if (GstElement* demux = gst_bin_get_by_name(GST_BIN(inst.pipeline), "d");
      demux != nullptr) {
    g_signal_connect(demux, "pad-added", G_CALLBACK(&demux_pad_added_order),
                     &inst);
    GstIterator* pads = gst_element_iterate_src_pads(demux);
    GValue item = G_VALUE_INIT;
    bool done = false;
    while (!done) {
      switch (gst_iterator_next(pads, &item)) {
        case GST_ITERATOR_OK:
          demux_pad_added_order(demux, GST_PAD(g_value_get_object(&item)),
                                &inst);
          g_value_reset(&item);
          break;
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync(pads);
          break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
          done = true;
          break;
      }
    }
    g_value_unset(&item);
    gst_iterator_free(pads);
    gst_object_unref(demux);
  }
  GstElement* mux = gst_bin_get_by_name(GST_BIN(inst.pipeline), "mux");
  if (mux == nullptr) {
    return;
  }
  gst_object_unref(mux);
  attach_mux_sink_probes(inst);

  mux = gst_bin_get_by_name(GST_BIN(inst.pipeline), "mux");
  if (mux == nullptr) {
    return;
  }
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
    } else if (arg == "--measured-kbps") {
      opts.measured_kbps = std::atoi(next().c_str());
    } else if (arg == "--sink-restart-cmd") {
      opts.sink_restart_cmd = next();
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
  es_clip es_media;
  const int fixture_seconds = std::max(opts.duration_s, 6);
  if (is_es_mode(opts.run_mode)) {
    // Standby enabled: the stage has already demuxed and parsed, so what the
    // output consumes is a stream of access units with their own timestamps.
    // Generated here with the same elements the TS fixtures use.
    es_media = make_es_clip(opts.width, opts.height, opts.bitrate_kbps,
                            opts.fps, fixture_seconds);
    if (es_media.video.empty() || es_media.duration_ns <= 0) {
      std::fprintf(stderr, "FAIL: could not generate the ES fixture\n");
      return 1;
    }
    if (es_media.audio.empty()) {
      std::fprintf(stderr, "FAIL: could not generate the audio ES fixture\n");
      return 1;
    }
  } else if (is_decode_mode(opts.run_mode)) {
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
    if (opts.run_mode == mode::copy_non_aac) {
      ts_clip = make_h264_ac3_ts(opts.width, opts.height, opts.bitrate_kbps,
                                 fixture_seconds);
    } else if (has_audio(opts.run_mode)) {
      ts_clip = make_h264_aac_ts(opts.width, opts.height, opts.bitrate_kbps,
                                 fixture_seconds,
                                 opts.run_mode == mode::copy_audio_first);
    } else {
      ts_clip = make_h264_ts(opts.width, opts.height, opts.bitrate_kbps,
                             fixture_seconds);
    }
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

  // The order the demuxer announces this clip's pads in, measured on the clip
  // itself: an "audio-first" mode is only that if the audio pad really comes
  // first, and the copy template must publish either way.
  std::vector<std::string> pad_order;
  if (is_copy_mode(opts.run_mode) && !is_es_mode(opts.run_mode)) {
    pad_order = probe_demux_pad_order(ts_clip);
    if (pad_order.empty()) {
      std::fprintf(stderr, "FAIL: tsdemux announced no pads for the fixture\n");
      return 1;
    }
  }

  // ---- the real output, configured the way /start configures it ----
  ts_ring ts_ring_instance {ts_ring::size_for_bandwidth(20000)};
  frame_ring frame_ring_instance {frame_ring::k_min_bytes};
  // copy-es: the two rings the standby stage publishes into. They exist only for
  // that mode; every other mode leaves es_sources empty, which is the ts_ring
  // path the output has always taken.
  std::unique_ptr<es_ring> es_video;
  std::unique_ptr<es_ring> es_audio;
  es_sources es_rings;
  if (is_es_mode(opts.run_mode)) {
    es_video = std::make_unique<es_ring>(
        es_ring::size_for_bps(opts.bitrate_kbps));
    es_audio = std::make_unique<es_ring>(es_ring::size_for_bps(128));
    es_rings.video = es_video.get();
    es_rings.audio = es_audio.get();
    es_rings.video_codec = codec::h264;
  }
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
              },
              {},
              es_rings};

  std::printf("=== RTMP publish harness: mode=%s ===\n", mode_name(opts.run_mode));
  std::printf("build commit   %s\n", OBR_SOURCE_COMMIT);
  std::printf("publish        %s\n", url.c_str());
  std::printf("server stat    %s\n", opts.stat_url.c_str());
  std::printf("server baseline bytes_in %lld\n", *baseline.bytes_in);
  if (!pad_order.empty()) {
    std::string joined;
    for (const std::string& name : pad_order) {
      joined += (joined.empty() ? "" : " ") + name;
    }
    std::printf("fixture pad order    %s\n", joined.c_str());
  }
  std::fflush(stdout);

  if (!out.start()) {
    std::printf("RESULT         FAIL (output refused to start)\n");
    return 1;
  }

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::seconds {opts.duration_s};

  std::uint64_t frames_pushed = 0;
  std::size_t ts_cursor = 0;
  std::size_t es_video_cursor = 0;
  std::size_t es_audio_cursor = 0;
  std::int64_t es_epoch_ns = 0;
  std::int64_t es_audio_epoch_ns = 0;
  std::uint64_t es_aus_pushed = 0;
  bool instrumented = false;
  bool server_grew = false;
  std::optional<long long> last_server = baseline.bytes_in;
  auto next_poll = started;

  // Media pacing for the whole run, in elapsed seconds since `started`. Any loop
  // that keeps the output alive calls this: a copy output takes TS bytes, a
  // transcode output takes whole NV12 frames plus (for the RTMP audio branch)
  // AAC-in-TS on the same ts_ring, and transcode-av1 only feeds TS because its
  // frame ring is filled by the real decode stage. copy-es feeds AU rings.
  const auto pump = [&](double elapsed) -> void {
    if (is_es_mode(opts.run_mode)) {
      // Publish AUs paced by their own PTS, exactly as the standby stage does:
      // an AU becomes visible when its timestamp is due, and the clip repeats by
      // advancing the epoch so PTS stays monotonic for the whole run.
      const std::int64_t now_ns = static_cast<std::int64_t>(elapsed * 1e9);
      for (;;) {
        if (es_video_cursor >= es_media.video.size()) {
          es_video_cursor = 0;
          es_epoch_ns += es_media.duration_ns;
        }
        const es_au& au = es_media.video[es_video_cursor];
        if (au.pts_ns + es_epoch_ns > now_ns) {
          break;
        }
        es_ring::au_meta meta;
        meta.pts_ns = au.pts_ns + es_epoch_ns;
        meta.dts_ns = meta.pts_ns;
        meta.keyframe = au.keyframe;
        if (!es_video->write(meta, au.bytes.data(), au.bytes.size())) {
          break;
        }
        ++es_video_cursor;
        ++es_aus_pushed;
      }
      for (;;) {
        if (es_audio_cursor >= es_media.audio.size()) {
          es_audio_cursor = 0;
          es_audio_epoch_ns += es_media.audio_duration_ns;
        }
        const es_au& au = es_media.audio[es_audio_cursor];
        if (au.pts_ns + es_audio_epoch_ns > now_ns) {
          break;
        }
        es_ring::au_meta meta;
        meta.pts_ns = au.pts_ns + es_audio_epoch_ns;
        meta.dts_ns = meta.pts_ns;
        meta.keyframe = false;
        if (!es_audio->write(meta, au.bytes.data(), au.bytes.size())) {
          break;
        }
        ++es_audio_cursor;
        ++es_aus_pushed;
      }
      return;
    }
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
      return;
    }
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
  };

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

    pump(elapsed);

    if (now >= next_poll) {
      next_poll = now + std::chrono::milliseconds {500};
      // Pick up a mux sink pad that did not exist when the run started: the
      // output requests the audio pad only once the source TS shows audio.
      attach_mux_sink_probes(inst);
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

  // ---- reconnect scenario -----------------------------------------------
  // A drop is only meaningful once a publish exists, so this runs after the
  // server counter proved one: the sink is restarted under the running output and
  // the rebuild has to reach publish again.
  const bool reconnect_mode = opts.run_mode == mode::reconnect;
  bool reconnect_ok = true;
  std::string reconnect_failure;
  int steady_kbps = 0;
  int bootstrap_kbps = 0;
  int pre_drop_kbps = 0;
  bool saw_bootstrap = false;
  bool sink_dropped = false;
  bool republished = false;
  bool ramped_back = false;
  std::uint64_t reconnects_before = 0;
  std::uint64_t reconnects_during = 0;
  long long post_drop_bytes = 0;

  if (reconnect_mode) {
    if (!server_grew) {
      reconnect_ok = false;
      reconnect_failure = "no publish to drop: the first publish never reached "
                          "the server";
    } else if (opts.sink_restart_cmd.empty()) {
      reconnect_ok = false;
      reconnect_failure = "reconnect mode needs --sink-restart-cmd";
    }
  }

  if (reconnect_mode && reconnect_ok) {
    // The encoder has to be at the measured-link target before the drop, so the
    // rebuild's bootstrap rate is a visible fall back and the ramp back after the
    // republish is observable.
    steady_kbps =
        derive_transcode_bitrate_kbps(opts.measured_kbps, opts.bitrate_kbps);
    bootstrap_kbps =
        derive_transcode_bootstrap_kbps(opts.measured_kbps, opts.bitrate_kbps);
    if (bootstrap_kbps >= steady_kbps) {
      // Without a gap between the two the rebuild's rate proves nothing: a
      // bootstrap equal to the measured target is the defect, not the subject.
      reconnect_ok = false;
      reconnect_failure = std::format(
          "the scenario cannot distinguish the connect bootstrap ({} kbps) from "
          "the measured target ({} kbps)",
          bootstrap_kbps, steady_kbps);
    }
  }

  if (reconnect_mode && reconnect_ok) {
    out.retarget_bitrate(opts.measured_kbps);
    const auto ramp_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {5};
    while (std::chrono::steady_clock::now() < ramp_deadline
           && out.bitrate_kbps() != steady_kbps)
    {
      pump(std::chrono::duration<double>(std::chrono::steady_clock::now()
                                         - started)
               .count());
      std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    pre_drop_kbps = out.bitrate_kbps();
    if (pre_drop_kbps != steady_kbps) {
      reconnect_ok = false;
      reconnect_failure = std::format(
          "encoder stayed at {} kbps after a {} kbps measurement; the measured "
          "target {} kbps was never applied",
          pre_drop_kbps, opts.measured_kbps, steady_kbps);
    } else {
      // Taken before the drop: the attempt the drop kills and the attempts that
      // retry a destination which is not back yet are what the bound is about.
      reconnects_before = out.reconnects();
      std::printf("reconnect            dropping the publisher: %s\n",
                  opts.sink_restart_cmd.c_str());
      std::fflush(stdout);
      if (std::system(opts.sink_restart_cmd.c_str()) != 0) {
        reconnect_ok = false;
        reconnect_failure = "the sink restart command failed";
      }
    }
  }

  if (reconnect_mode && reconnect_ok) {
    const auto republish_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {45};
    auto next_stat = std::chrono::steady_clock::now();
    while (!republished && std::chrono::steady_clock::now() < republish_deadline) {
      const auto now = std::chrono::steady_clock::now();
      pump(std::chrono::duration<double>(now - started).count());
      // The bootstrap rate is applied by every rebuild, so seeing it on the
      // running encoder is the rebuild having started at the capped rate rather
      // than the measured target.
      if (out.bitrate_kbps() == bootstrap_kbps) {
        saw_bootstrap = true;
      }
      if (now >= next_stat) {
        next_stat = now + std::chrono::milliseconds {250};
        const stat_probe probe = fetch_server_bytes(opts.stat_url);
        // The restarted sink holds no stream until the output publishes again, so
        // an empty counter is the destination being gone, not the old session
        // still counting.
        if (!probe.bytes_in || *probe.bytes_in == 0) {
          sink_dropped = true;
        } else if (sink_dropped && *probe.bytes_in >= k_server_bytes_proof) {
          post_drop_bytes = *probe.bytes_in;
          republished = true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    reconnects_during = out.reconnects() - reconnects_before;
    if (!republished) {
      reconnect_ok = false;
      reconnect_failure = std::format(
          "no republish within 45 s of the drop ({} reconnect attempt(s), state "
          "{}, last error {})",
          reconnects_during, out.state_name(),
          out.last_error().empty() ? "(none)" : out.last_error());
    } else {
      // Settle on the republished session: the measured-link rate must come back,
      // otherwise the cap would have replaced live tracking instead of bounding
      // the connect phase.
      const auto settle_until = std::chrono::steady_clock::now()
          + std::chrono::seconds {2};
      while (std::chrono::steady_clock::now() < settle_until) {
        pump(std::chrono::duration<double>(std::chrono::steady_clock::now()
                                           - started)
                 .count());
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
      }
      ramped_back = out.bitrate_kbps() == steady_kbps
          && std::string_view {out.state_name()} == "running";
      if (!ramped_back) {
        reconnect_ok = false;
        reconnect_failure = std::format(
            "after republishing the encoder is at {} kbps in state {}; the "
            "measured target was not restored",
            out.bitrate_kbps(), out.state_name());
      } else if (reconnects_during > 4) {
        // A sink that is still restarting costs one attempt per refused connect,
        // so a handful is a destination coming back. The defect this bounds is
        // an output that retries past the connect window without republishing,
        // which the republish check above already fails.
        reconnect_ok = false;
        reconnect_failure = std::format(
            "the rebuild needed {} attempts to republish",
            reconnects_during);
      }
    }
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
  const std::uint64_t bytes_published = out.bytes_published_total();
  const std::uint64_t frames_published = out.frames_published_total();
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
  std::vector<std::string> demux_pads;
  {
    std::lock_guard<std::mutex> guard(inst.mutex);
    demux_pads = inst.demux_pads;
  }
  bool ok = server_grew;
  std::string mode_failure;
  if (ok && server_grew && has_audio(opts.run_mode) && mux_audio_final == 0) {
    ok = false;
    mode_failure = "audio never reached the muxer";
  }
  if (ok && server_grew && !has_audio(opts.run_mode) && mux_audio_final != 0) {
    ok = false;
    mode_failure = "audio reached the muxer in a video-only mode";
  }
  // The audio-first mode exists to feed the order that used to break the copy
  // template. If the fixture did not actually expose its audio pad first, the
  // run is a second video-first run and proves nothing about pad ordering.
  if (ok && server_grew && wants_audio_first(opts.run_mode)
      && (demux_pads.empty()
          || demux_pads.front().find("(audio/") == std::string::npos)) {
    ok = false;
    mode_failure = "the fixture did not expose the audio pad first";
  }
  // The decode mode exists to prove the AV1 -> NV12 -> H.264 -> RTMP chain, so
  // publishing without the decode stage having produced frames would be a pass
  // for the wrong reason.
  if (ok && reconnect_mode && !reconnect_ok) {
    ok = false;
    mode_failure = reconnect_failure;
  }
  if (ok && is_decode_mode(opts.run_mode)) {
    if (decoded_frames == 0) {
      ok = false;
      mode_failure = "decode stage published no frame to the frame ring";
    } else if (decode_counters.failed || decode_counters.stalled) {
      ok = false;
      mode_failure = "decode stage reported failed/stalled";
    }
  }

  // copy-es exists to prove the ES-fed copy template publishes: AUs in, muxer
  // buffers out, server bytes on top. A publish with no buffer through the
  // muxer would mean something other than the template produced those bytes.
  if (ok && server_grew && is_es_mode(opts.run_mode)) {
    if (es_aus_pushed == 0) {
      ok = false;
      mode_failure = "no access unit was ever published into the es_ring";
    } else if (inst.mux_src_buffers.load() == 0) {
      ok = false;
      mode_failure = "the muxer emitted no buffer";
    } else if (inst.mux_video_buffers.load() == 0) {
      ok = false;
      mode_failure = "no video AU reached the muxer";
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
    // The clip must have the pad order the mode name states, so a "video-first"
    // mode cannot silently run an audio-first clip and pass on either routing.
    // copy-es has no demuxer, so it has no pad order to check.
    if (ok && is_copy_mode(opts.run_mode) && !is_es_mode(opts.run_mode)) {
      const bool audio_first = pad_order.front().rfind("audio", 0) == 0;
      const bool want_audio_first = opts.run_mode == mode::copy_audio_first;
      if (audio_first != want_audio_first) {
        ok = false;
        mode_failure = audio_first
            ? "fixture announces its audio pad first but the mode says video"
            : "fixture announces its video pad first but the mode says audio";
      }
    }
    // Copy path identity: the video pad is fed by h264parse and the audio pad by
    // aacparse, so the format each stream carries names the branch it reached.
    // H.264 arriving as AVC on the video pad and AAC arriving as raw on the
    // audio pad cannot happen if either stream was linked to the other parser —
    // order cannot be what decided the routing.
    if (ok && server_grew && is_copy_mode(opts.run_mode)) {
      if (inst.video_caps.find("video/x-h264") == std::string::npos
          || inst.video_caps.find("stream-format=(string)avc")
              == std::string::npos)
      {
        ok = false;
        mode_failure = "muxer video pad did not receive h264parse output";
      } else if (mux_audio_final != 0
                 && (inst.audio_caps.find("audio/mpeg") == std::string::npos
                     || inst.audio_caps.find("stream-format=(string)raw")
                         == std::string::npos))
      {
        ok = false;
        mode_failure = "muxer audio pad did not receive aacparse output";
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
  if (is_es_mode(opts.run_mode)) {
    std::printf("es aus pushed        %llu (au drops %llu)\n",
                static_cast<unsigned long long>(es_aus_pushed),
                static_cast<unsigned long long>(out.dropped_aus()));
  }
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
  if (!demux_pads.empty()) {
    std::string order;
    for (const auto& entry : demux_pads) {
      order += (order.empty() ? "" : ", ") + entry;
    }
    std::printf("demux pad order      %s\n", order.c_str());
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
  if (reconnect_mode) {
    std::printf("connect bootstrap    %d kbps (measured target %d kbps)\n",
                bootstrap_kbps, steady_kbps);
    std::printf("pre-drop bitrate     %d kbps\n", pre_drop_kbps);
    std::printf("bootstrap on rebuild %s\n", saw_bootstrap ? "yes" : "no");
    std::printf("server dropped       %s\n", sink_dropped ? "yes" : "no");
    if (republished) {
      std::printf("republished          yes (%lld server bytes_in after the drop)\n",
                  post_drop_bytes);
    } else {
      std::printf("republished          no\n");
    }
    std::printf("ramped back          %s\n", ramped_back ? "yes" : "no");
    std::printf("reconnect attempts   %llu\n",
                static_cast<unsigned long long>(reconnects_during));
  }
  std::printf("output published     %llu bytes in %llu sink buffers\n",
              static_cast<unsigned long long>(bytes_published),
              static_cast<unsigned long long>(frames_published));
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
    std::printf("audio pad            never requested: the TS carries no audio, "
                "and video still published\n");
  }

  std::printf("RESULT               %s\n", failed ? "FAIL" : "PASS");
  if (!mode_failure.empty()) {
    std::printf("check failed         %s\n", mode_failure.c_str());
  }
  if (failed) {
    std::printf("expected             mode %s to publish: >= %lld new bytes "
                "at %s (handshake is not proof); server delta %lld\n",
                mode_name(opts.run_mode), k_server_bytes_proof,
                opts.stat_url.c_str(), server_delta);
  }
  return failed ? 1 : 0;
}
