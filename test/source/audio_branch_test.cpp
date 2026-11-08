// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Audio branch of the RTMP transcode output.
//
// The published audio is DECLARED, not discovered: the branch states AAC-LC
// 48 kHz stereo with the AudioSpecificConfig in codec_data, and the source is
// whatever AAC the carried MPEG-TS holds. Three properties are asserted, each
// one a way this branch has failed before:
//
//   * the template states the declared caps and does not link the muxer's audio
//     pad at parse time;
//   * an AAC-in-MPEG-TS source really converts into those caps. tsdemux exposes
//     `audio/mpeg, mpegversion=4, stream-format=adts` with no rate, channel
//     count or codec_data, so the claim being tested is that aacparse turns
//     exactly that into the declared raw AAC-LC caps;
//   * a video-only source neither exposes an audio pad nor stalls the branch:
//     the demux's video pad is sunk into the drop funnel and the branch runs to
//     EOS. A pad left unlinked kills the pipeline with "not-linked", and a
//     request pad handed to flvmux without buffers holds the muxer silent.

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <gst/gst.h>

#include "fanout/output.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto has(const std::string& text, std::string_view needle) -> bool
{
  return text.find(needle) != std::string::npos;
}

// The capsfilter's caps as the template states them, so the negotiation test
// below asserts against the product's declaration instead of a copy.
auto declared_audio_caps(const std::string& tmpl) -> std::string
{
  const std::string start_marker = "caps=audio/mpeg";
  const std::size_t start = tmpl.find(start_marker);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t end_marker = tmpl.find("codec_data=(buffer)1190", start);
  if (end_marker == std::string::npos) {
    return {};
  }
  const std::size_t end = end_marker + std::strlen("codec_data=(buffer)1190");
  return tmpl.substr(start + std::strlen("caps="), end - start - 5);
}

auto make_temp_path(const char* stem) -> std::string
{
  std::string pattern = std::string(g_get_tmp_dir()) + "/" + stem + "-XXXXXX.ts";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const int fd = ::mkstemps(buffer.data(), 3);  // keeps the .ts suffix
  if (fd < 0) {
    return {};
  }
  ::close(fd);
  return std::string(buffer.data());
}

struct bus_end
{
  bool eos = false;
  bool error = false;
  std::string error_text;
};

// Runs a pipeline to EOS (or its first error) and tears it down. `settled` lets
// a caller stop as soon as the branch it cares about has seen what it needs: an
// idle branch that never ends keeps the pipeline from ever posting EOS.
auto run_to_end(GstElement* pipeline, unsigned seconds,
                const std::function<bool()>& settled = {}) -> bus_end
{
  bus_end result;
  GstBus* bus = gst_element_get_bus(pipeline);
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  const auto deadline = std::chrono::steady_clock::now()
      + std::chrono::seconds(seconds);
  for (;;) {
    if (settled && settled()) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    GstMessage* msg =
        gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND,
                                   static_cast<GstMessageType>(
                                       GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (msg == nullptr) {
      continue;  // quiet slice: re-check the settle predicate
    }
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
      result.eos = true;
    } else {
      GError* err = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &err, &debug);
      result.error = true;
      result.error_text = (err != nullptr && err->message != nullptr)
          ? err->message : "unknown";
      if (err != nullptr) {
        g_error_free(err);
      }
      g_free(debug);
    }
    gst_message_unref(msg);
    if (result.eos || result.error) {
      break;
    }
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);
  gst_object_unref(bus);
  return result;
}

// Mirrors the product wiring: the demux is left unchained and its pads are
// routed by the pad-added signal — audio into the AAC queue, every other pad
// into the drop funnel.
struct branch_state
{
  GstElement* aq = nullptr;
  GstElement* adrop = nullptr;
  GstElement* acaps = nullptr;
  std::atomic<int> audio_pads {0};
  std::atomic<int> dropped_pads {0};
  std::atomic<int> dropped_buffers {0};
  std::atomic<int> acaps_buffers {0};
  std::atomic_bool acaps_eos {false};
  std::atomic_bool drop_eos {false};
  std::mutex mutex;
  std::string acaps_caps;
};

auto drop_probe(GstPad* pad, GstPadProbeInfo* info, gpointer data)
    -> GstPadProbeReturn;

auto link_branch_pad(GstElement* /*demux*/, GstPad* pad, gpointer data) -> void
{
  auto* state = static_cast<branch_state*>(data);
  bool audio = false;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps != nullptr) {
    audio = gst_structure_has_name(gst_caps_get_structure(caps, 0),
                                   "audio/mpeg") == TRUE;
    gst_caps_unref(caps);
  }
  GstPad* sink = nullptr;
  if (audio) {
    sink = gst_element_get_static_pad(state->aq, "sink");
    ++state->audio_pads;
  } else {
    sink = gst_element_request_pad_simple(state->adrop, "sink_%u");
    ++state->dropped_pads;
    if (sink != nullptr) {
      gst_pad_add_probe(sink,
                        static_cast<GstPadProbeType>(
                            GST_PAD_PROBE_TYPE_BUFFER
                            | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                        &drop_probe, state, nullptr);
    }
  }
  if (sink != nullptr && !gst_pad_is_linked(sink)) {
    gst_pad_link(pad, sink);
  }
  if (sink != nullptr) {
    gst_object_unref(sink);
  }
}

// The sunk video pad: buffers prove the demux's non-audio stream had somewhere
// to go, which is what keeps tsdemux from failing the branch with not-linked.
auto drop_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer data)
    -> GstPadProbeReturn
{
  auto* state = static_cast<branch_state*>(data);
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS) {
      state->drop_eos.store(true);
    }
    return GST_PAD_PROBE_PASS;
  }
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
    ++state->dropped_buffers;
  }
  return GST_PAD_PROBE_PASS;
}

auto acaps_probe(GstPad* pad, GstPadProbeInfo* info, gpointer data)
    -> GstPadProbeReturn
{
  auto* state = static_cast<branch_state*>(data);
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS) {
      state->acaps_eos.store(true);
    }
    return GST_PAD_PROBE_PASS;
  }
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
    return GST_PAD_PROBE_PASS;
  }
  ++state->acaps_buffers;
  if (state->acaps_caps.empty()) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps != nullptr) {
      gchar* text = gst_caps_to_string(caps);
      if (text != nullptr) {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->acaps_caps.empty()) {
          state->acaps_caps = text;
        }
        g_free(text);
      }
      gst_caps_unref(caps);
    }
  }
  return GST_PAD_PROBE_PASS;
}

// The branch exactly as the transcode template writes it, fed from a file
// instead of the audio appsrc, so what negotiates is the product's own caps.
auto branch_pipeline(const std::string& ts_path, const std::string& caps,
                     branch_state& state) -> GstElement*
{
  const std::string text =
      "filesrc location=" + ts_path
      + " ! tsparse set-timestamps=true alignment=7 ! tsdemux name=ad "
        "funnel name=adrop ! fakesink sync=false async=false "
        "queue name=aq ! aacparse ! capsfilter name=acaps caps=" + caps
      // async=false on both sinks: a filesrc pipeline waits for every sink to
      // preroll, and the branch that carries no data for the source at hand
      // never would -- which would hold the whole pipeline in PREROLLING.
      + " ! fakesink sync=false async=false";
  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(text.c_str(), &err);
  if (err != nullptr) {
    std::fprintf(stderr, "  branch parse error: %s\n", err->message);
    g_error_free(err);
  }
  if (pipeline == nullptr) {
    return nullptr;
  }
  state.aq = gst_bin_get_by_name(GST_BIN(pipeline), "aq");
  state.adrop = gst_bin_get_by_name(GST_BIN(pipeline), "adrop");
  state.acaps = gst_bin_get_by_name(GST_BIN(pipeline), "acaps");
  if (state.aq == nullptr || state.adrop == nullptr || state.acaps == nullptr) {
    gst_object_unref(pipeline);
    return nullptr;
  }
  GstElement* demux = gst_bin_get_by_name(GST_BIN(pipeline), "ad");
  if (demux == nullptr) {
    gst_object_unref(pipeline);
    return nullptr;
  }
  g_signal_connect(demux, "pad-added", G_CALLBACK(&link_branch_pad), &state);
  gst_object_unref(demux);
  GstPad* caps_src = gst_element_get_static_pad(state.acaps, "src");
  if (caps_src != nullptr) {
    gst_pad_add_probe(caps_src,
                      static_cast<GstPadProbeType>(
                          GST_PAD_PROBE_TYPE_BUFFER
                          | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                      &acaps_probe, &state, nullptr);
    gst_object_unref(caps_src);
  }
  return pipeline;
}

auto release_branch(branch_state& state) -> void
{
  for (GstElement* element : {state.aq, state.adrop, state.acaps}) {
    if (element != nullptr) {
      gst_object_unref(element);
    }
  }
  state.aq = nullptr;
  state.adrop = nullptr;
  state.acaps = nullptr;
}

auto run_branch(const std::string& ts_path, const std::string& caps,
                branch_state& state) -> bus_end
{
  GstElement* pipeline = branch_pipeline(ts_path, caps, state);
  if (pipeline == nullptr) {
    std::fprintf(stderr, "FAIL: could not build the audio branch\n");
    std::exit(1);
  }
  const bus_end end = run_to_end(
      pipeline, 30, [&state]() -> bool {
        // The video-only source has 60 frames: seeing most of them at the
        // funnel means the demux worked through the clip without failing.
        return state.acaps_eos.load() || state.drop_eos.load()
            || state.dropped_buffers.load() > 50;
      });
  gst_object_unref(pipeline);
  std::printf("  branch: eos=%d error=%d%s%s audio_pads=%d dropped_pads=%d "
              "acaps_buffers=%d acaps_eos=%d drop_eos=%d "
              "dropped_buffers=%d\n",
              end.eos, end.error,
              end.error ? " (" : "", end.error ? end.error_text.c_str() : "",
              static_cast<int>(state.audio_pads.load()),
              static_cast<int>(state.dropped_pads.load()),
              static_cast<int>(state.acaps_buffers.load()),
              static_cast<int>(state.acaps_eos.load()),
              static_cast<int>(state.drop_eos.load()),
              static_cast<int>(state.dropped_buffers.load()));
  return end;
}

// An AAC-only MPEG-TS in the shape the product's encoder produces: ADTS AAC
// pinned for mpegtsmux, one aligned TS per buffer.
auto make_aac_ts() -> std::string
{
  const std::string path = make_temp_path("obr-audio-branch-aac");
  if (path.empty()) {
    return {};
  }
  const int frames = (48000 / 1024) * 2;  // 2 s of 1024-sample AAC buffers
  GstElement* pipeline = gst_parse_launch(
      ("audiotestsrc is-live=false num-buffers=" + std::to_string(frames)
       + " wave=sine ! audio/x-raw,rate=48000,channels=2 "
         "! avenc_aac bitrate=128000 ! aacparse "
         "! audio/mpeg,mpegversion=4,stream-format=adts,framed=true "
         "! mpegtsmux alignment=7 ! filesink location=" + path)
          .c_str(),
      nullptr);
  if (pipeline == nullptr) {
    return {};
  }
  const bus_end end = run_to_end(pipeline, 30);
  gst_object_unref(pipeline);
  if (!end.eos || end.error) {
    std::fprintf(stderr, "FAIL: could not build the AAC-in-MPEG-TS fixture%s%s\n",
                 end.error ? ": " : "",
                 end.error ? end.error_text.c_str() : "");
    return {};
  }
  return path;
}

// A video-only MPEG-TS: the shape a source with no audio produces. No audio pad
// exists in it, which is the condition the branch must survive.
auto make_video_ts() -> std::string
{
  const std::string path = make_temp_path("obr-audio-branch-video");
  if (path.empty()) {
    return {};
  }
  GstElement* pipeline = gst_parse_launch(
      ("videotestsrc is-live=false num-buffers=60 "
       "! video/x-raw,format=NV12,width=320,height=180,framerate=30/1 "
       "! x264enc tune=zerolatency key-int-max=30 ! h264parse "
         "config-interval=-1 ! mpegtsmux alignment=7 ! filesink location="
       + path)
          .c_str(),
      nullptr);
  if (pipeline == nullptr) {
    return {};
  }
  const bus_end end = run_to_end(pipeline, 30);
  gst_object_unref(pipeline);
  if (!end.eos || end.error) {
    std::fprintf(stderr, "FAIL: could not build the video-only MPEG-TS fixture%s%s\n",
                 end.error ? ": " : "",
                 end.error ? end.error_text.c_str() : "");
    return {};
  }
  return path;
}
}  // namespace

auto main() -> int
{
  gst_init(nullptr, nullptr);

  transcode_config h264;
  h264.target = transcode_target::h264;
  const std::string tmpl =
      output_template(output_proto::rtmp, h264, "vah264enc");

  // 1) The declared caps are the published contract.
  const std::string caps = declared_audio_caps(tmpl);
  expect(!caps.empty(), "the audio branch declares capsfilter caps");
  expect(has(caps, "audio/mpeg") && has(caps, "mpegversion=4"),
         "declared audio caps are MPEG-4 audio");
  expect(has(caps, "stream-format=raw"),
         "declared audio caps are raw AAC, not ADTS (FLV carries raw)");
  expect(has(caps, "rate=48000") && has(caps, "channels=2"),
         "declared audio caps are 48 kHz stereo");
  expect(has(caps, "codec_data=(buffer)1190"),
         "declared audio caps carry the AAC-LC 48 kHz stereo AudioSpecificConfig");
  expect(has(tmpl, "aacparse ! capsfilter name=acaps"),
         "the audio branch parses whatever AAC the source TS carries");

  // 2) Nothing is pre-linked to the muxer: video publishes on its first
  //    encoded frame whether or not the source ever exposes an audio pad.
  {
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(tmpl.c_str(), &err);
    expect(pipeline != nullptr && err == nullptr,
           "the h264 rtmp transcode template parses");
    GstElement* mux = gst_bin_get_by_name(GST_BIN(pipeline), "mux");
    expect(mux != nullptr, "the template has a muxer");
    int sink_pads = 0;
    int audio_pads = 0;
    GstIterator* it = gst_element_iterate_sink_pads(mux);
    GValue item = G_VALUE_INIT;
    bool done = false;
    while (!done) {
      switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
          auto* pad = GST_PAD(g_value_get_object(&item));
          const char* name = GST_PAD_NAME(pad);
          ++sink_pads;
          if (name != nullptr && g_str_has_prefix(name, "audio") == TRUE) {
            ++audio_pads;
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
    gst_object_unref(pipeline);
    // A muxer sink-pad count without an audio pad is the invariant, not a
    // detail: flvmux emits nothing while a requested audio pad carries no
    // buffers, so a pre-linked audio pad is what holds video back.
    expect(sink_pads == 1 && audio_pads == 0,
           "the muxer has only the video sink pad at parse time");
  }

  // 3) What tsdemux exposes converts into the declared caps, and buffers flow.
  const std::string aac_ts = make_aac_ts();
  expect(!aac_ts.empty(), "generated the AAC-in-MPEG-TS fixture");
  {
    branch_state state;
    const bus_end end = run_branch(aac_ts, caps, state);
    std::string negotiated;
    {
      std::lock_guard<std::mutex> guard(state.mutex);
      negotiated = state.acaps_caps;
    }
    release_branch(state);
    std::remove(aac_ts.c_str());
    std::printf("  negotiated audio caps: %s\n",
                negotiated.empty() ? "(none)" : negotiated.c_str());
    // The funnel has no sink pad on an audio-only source, so it never forwards
    // EOS: the audio branch's own EOS is what says the whole AAC stream was
    // consumed.
    expect(!end.error, "the audio branch reported no error on AAC");
    expect(state.acaps_eos.load(),
           "the audio branch consumed the AAC stream to its end");
    expect(state.audio_pads.load() == 1,
           "the demux exposed exactly one audio pad");
    expect(state.acaps_buffers.load() > 0,
           "AAC buffers reached the declared capsfilter");
    expect(has(negotiated, "stream-format=(string)raw")
               && has(negotiated, "rate=(int)48000")
               && has(negotiated, "channels=(int)2")
               && has(negotiated, "codec_data=(buffer)1190"),
           "the negotiated caps are the declared AAC-LC 48 kHz stereo raw");
  }

  // 4) A video-only source cannot gate the branch: no audio pad, the video pad
  //    sunk, and the branch ends instead of failing with not-linked.
  const std::string video_ts = make_video_ts();
  expect(!video_ts.empty(), "generated the video-only MPEG-TS fixture");
  {
    branch_state state;
    const bus_end end = run_branch(video_ts, caps, state);
    release_branch(state);
    std::remove(video_ts.c_str());
    // The AAC branch is idle and never ends, so the pipeline as a whole stays
    // open; what matters is that the video side drained to EOS with no error
    // rather than stalling the run.
    expect(!end.error, "the branch reported no error on a video-only source");
    expect(state.dropped_buffers.load() >= 50,
           "the demux's video pad carried its buffers into the drop funnel");
    expect(state.drop_eos.load(),
           "the demux's video pad drained to EOS through the drop funnel");
    expect(state.audio_pads.load() == 0,
           "a video-only source exposed no audio pad");
    expect(state.dropped_pads.load() > 0,
           "the demux's video pad was sunk into the drop funnel");
    expect(state.acaps_buffers.load() == 0,
           "no AAC buffer reached the declared capsfilter");
  }

  std::puts("audio_branch_test: ALL OK");
  return 0;
}
