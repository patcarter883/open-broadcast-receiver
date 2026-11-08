// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// slate_pipeline_test — the standby slate renderer (plan B1).
//
// Two halves, in this order:
//   1. The strings. Every pipeline the renderer can build is asserted for the
//      elements and properties that make it correct, without running anything:
//      the encoder choice per codec, the parser, the ES framing caps, the
//      optional brand overlay, the 2*fps length, and the cache key.
//   2. One real render, at the smallest geometry that proves the chain links
//      (320x180@2, h264), skipped with an explanation when the box has no
//      software h264 encoder. It is the only place the GStreamer graph is
//      actually built, so the string assertions above cannot be the whole
//      story.
//
// Framework-free; exit 0 = pass.

#include <glib.h>
#include <gst/gst.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "standby/es_looper.h"
#include "standby/render.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto contains(const std::string& haystack, const std::string& needle,
              const char* what) -> void
{
  if (haystack.find(needle) == std::string::npos) {
    std::fprintf(stderr, "FAIL: %s\n  missing: %s\n  in: %s\n", what,
                 needle.c_str(), haystack.c_str());
    std::exit(1);
  }
}

auto present_none(const char*) -> bool
{
  return false;
}

auto make_present(std::vector<std::string> names)
{
  return [names](const char* name) -> bool {
    for (const auto& candidate : names) {
      if (candidate == name) {
        return true;
      }
    }
    return false;
  };
}

auto test_plan() -> void
{
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "obr_slate_pipeline_test";
  const std::filesystem::path brand = scratch / "brand.png";
  std::filesystem::create_directories(scratch);
  std::ofstream(brand) << "not a real png, only the path has to exist";

  standby::slate_request req;
  req.codec = "h264";
  req.width = 1280;
  req.height = 720;
  req.fps = 30;
  req.bitrate_kbps = 1500;
  req.logo_path = "/usr/share/open-broadcast/logo.png";
  req.brand_path = brand.string();
  req.text = "stream standby";
  req.out_dir = scratch.string();

  // --- h264, hardware present -------------------------------------------
  std::string err;

  const auto h264 = standby::plan_slate(req, make_present({"vah264enc", "x264enc"}), err);
  expect(h264.has_value(), "h264 plan builds");
  expect(h264->encoder == "vah264enc", "h264 prefers the hardware encoder");

  const std::string& v = h264->video_pipeline;
  expect(v.find("videotestsrc pattern=black num-buffers=60") != std::string::npos,
         "60 frames = 2 s at 30 fps");
  expect(v.find("width=1280") != std::string::npos
             && v.find("height=720") != std::string::npos,
         "geometry is the session geometry (caps equality downstream)");
  expect(v.find("framerate=30/1") != std::string::npos, "framerate");
  expect(v.find("format=I420") != std::string::npos, "generated as I420");
  expect(v.find("video/x-raw,format=NV12") != std::string::npos,
         "NV12 before the encoder (the VA encoders accept nothing else)");
  expect(v.find("gdkpixbufoverlay location=\"/usr/share/open-broadcast/logo.png\" offset-x=40 offset-y=40")
             != std::string::npos,
         "logo overlay at the configured inset");
  expect(v.find("gdkpixbufoverlay location=\"" + brand.string() + "\" offset-x=-40")
             != std::string::npos,
         "brand overlay, 40 px from the right edge");
  expect(v.find("textoverlay text=\"stream standby\" font-desc=\"Sans 48\"") != std::string::npos,
         "caption");
  expect(v.find("vah264enc bitrate=1500") != std::string::npos, "h264 encoder");
  expect(v.find("key-int-max=1") != std::string::npos,
         "the slate is all-intra: every AU is independently decodable");
  expect(v.find("h264parse config-interval=-1") != std::string::npos,
         "h264 parse with SPS/PPS on every keyframe");
  expect(v.find("video/x-h264,stream-format=byte-stream,alignment=au") != std::string::npos,
         "h264 ES is Annex-B byte-stream");
  expect(v.find("filesink location=\"" + h264->video_path + "\"") != std::string::npos,
         "video sink writes the cache path");
  expect(h264->video_path.ends_with(".h264"), "h264 extension");
  expect(h264->audio_path.ends_with(".aac"), "audio extension");
  expect(h264->key.size() == 16, "cache key is 16 hex chars");

  // Audio: silence, so the audio branch stays negotiated for the session.
  const std::string& a = h264->audio_pipeline;
  expect(a.find("audiotestsrc wave=silence num-buffers=60") != std::string::npos,
         "audio is two seconds of silence");
  // num-buffers is a count of buffers, so the buffer size is what makes it a
  // duration: 60 x (48000/30) samples = exactly 2 s.
  expect(a.find("samplesperbuffer=1600") != std::string::npos,
         "buffer size makes 2*fps buffers two seconds of audio");
  expect(a.find("audio/x-raw,rate=48000,channels=2") != std::string::npos,
         "AAC-LC 48 kHz stereo, as the live source declares");
  expect(a.find("avenc_aac bitrate=128000") != std::string::npos, "AAC encoder");
  expect(a.find("aacparse") != std::string::npos, "AAC parser");
  // aacparse's output format is negotiated; without this the file is written in
  // a raw format aacparse cannot read back (verified on 1.28: the audio branch
  // then has nothing to loop).
  expect(a.find("audio/mpeg,stream-format=adts,parsed=true") != std::string::npos,
         "the audio ES is pinned to ADTS");
  expect(a.find("filesink location=\"" + h264->audio_path + "\"") != std::string::npos,
         "audio sink writes the cache path");

  // --- the software fallback and the other two codecs -------------------
  {
    const auto plan = standby::plan_slate(req, make_present({"x264enc"}), err);
    expect(plan.has_value() && plan->encoder == "x264enc",
           "software h264 is chosen when the VA encoder is absent");
    expect(plan->video_pipeline.find("x264enc bitrate=1500") != std::string::npos,
           "x264 fragment");
  }
  {
    standby::slate_request h265 = req;
    h265.codec = "h265";
    const auto plan = standby::plan_slate(h265, make_present({"vah265enc"}), err);
    expect(plan.has_value(), "h265 plan builds");
    expect(plan->video_pipeline.find("vah265enc bitrate=1500") != std::string::npos,
           "h265 encoder");
    expect(plan->video_pipeline.find("h265parse config-interval=-1") != std::string::npos,
           "h265 parser");
    expect(plan->video_pipeline.find("video/x-h265,stream-format=byte-stream") != std::string::npos,
           "h265 ES is Annex-B byte-stream");
    expect(plan->video_path.ends_with(".h265"), "h265 extension");
  }
  {
    standby::slate_request av1 = req;
    av1.codec = "av1";
    const auto plan = standby::plan_slate(av1, make_present({"av1enc"}), err);
    expect(plan.has_value(), "av1 plan builds");
    // aom's av1enc calls the same quantity target-bitrate, in kbit/s.
    expect(plan->video_pipeline.find("av1enc target-bitrate=1500") != std::string::npos,
           "av1encoder and its bitrate property");
    expect(plan->video_pipeline.find("av1parse") != std::string::npos, "av1 parser");
    expect(plan->video_pipeline.find("keyframe-max-dist=1") != std::string::npos,
           "av1 slate is all-intra too (aom spells it keyframe-max-dist)");
    // av1parse has no config-interval; asking for one would fail the parse.
    expect(plan->video_pipeline.find("config-interval") == std::string::npos,
           "av1 does not ask for config-interval");
    expect(plan->video_pipeline.find("video/x-av1,stream-format=obu-stream,alignment=frame")
               != std::string::npos,
           "av1 ES is the obu-stream framing mpegtsmux/tsdemux carry");
    expect(plan->video_path.ends_with(".av1"), "av1 extension");
  }

  // --- the brand overlay is optional, never fatal -----------------------
  {
    standby::slate_request no_brand = req;
    no_brand.brand_path = (scratch / "does-not-exist.png").string();
    const auto plan = standby::plan_slate(no_brand, make_present({"x264enc"}), err);
    expect(plan.has_value(), "a missing brand image still plans");
    expect(!plan->brand_overlay, "a missing brand image is not overlaid");
    expect(plan->video_pipeline.find("does-not-exist.png") == std::string::npos,
           "the missing brand path never reaches the pipeline");
    // ... and it is a different cache key, because the pipeline differs.
    expect(plan->key != h264->key, "branding is part of the cache identity");

    standby::slate_request empty_brand = req;
    empty_brand.brand_path.clear();
    const auto plain = standby::plan_slate(empty_brand, make_present({"x264enc"}), err);
    expect(plain.has_value() && !plain->brand_overlay,
           "no brand configured = no overlay");
  }

  // --- refusals are explicit -------------------------------------------
  {
    standby::slate_request bad = req;
    bad.codec = "vp9";
    expect(!standby::plan_slate(bad, make_present({"x264enc"}), err).has_value(),
           "an unknown codec is refused");
    expect(!err.empty(), "the refusal carries a reason");

    bad = req;
    bad.width = 0;
    expect(!standby::plan_slate(bad, make_present({"x264enc"}), err).has_value(),
           "unknown geometry is refused rather than guessed");

    const auto none = standby::plan_slate(req, present_none, err);
    expect(!none.has_value(), "no encoder installed is a refusal");
    expect(err.find("vah264enc") != std::string::npos
               && err.find("x264enc") != std::string::npos,
           "the reason names the alternatives it tried");
  }

  // --- the cache key ----------------------------------------------------
  {
    standby::slate_request a_req = req;
    standby::slate_request b_req = req;
    expect(standby::slate_cache_key(a_req) == standby::slate_cache_key(b_req),
           "the same request has the same key");
    b_req.text = "different caption";
    expect(standby::slate_cache_key(a_req) != standby::slate_cache_key(b_req),
           "the caption is part of the key");
    b_req = req;
    b_req.width = 1920;
    expect(standby::slate_cache_key(a_req) != standby::slate_cache_key(b_req),
           "geometry is part of the key");
    const std::string key = standby::slate_cache_key(a_req);
    expect(key.size() == 16, "the key is truncated to 16 chars");
    for (const char c : key) {
      expect((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
             "the key is hex");
    }
  }

  std::filesystem::remove_all(scratch);
}

auto test_render() -> void
{
  // Only the software encoder is required: a real render here would otherwise
  // depend on a VA render node, which is a deployment concern, not this test's.
  if (gst_element_factory_find("x264enc") == nullptr) {
    std::puts("slate_pipeline_test: SKIP live render (no x264enc on this box)");
    return;
  }

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "obr_slate_render_test";
  std::filesystem::remove_all(dir);

  standby::slate_request req;
  req.codec = "h264";
  req.width = 320;
  req.height = 180;
  req.fps = 2;
  req.bitrate_kbps = 300;
  req.text = "stream standby";
  req.out_dir = dir.string();
  req.encoder_override = "x264enc";  // pin it: see above

  std::string err;
  const auto assets = standby::render_slate(req, err);
  if (!assets.has_value()) {
    std::fprintf(stderr, "FAIL: live slate render: %s\n", err.c_str());
    std::exit(1);
  }

  const auto size_of = [](const std::string& path) -> std::uintmax_t {
    std::error_code ec;
    return std::filesystem::file_size(path, ec);
  };
  expect(std::filesystem::exists(assets->video_path), "video file exists");
  expect(std::filesystem::exists(assets->audio_path), "audio file exists");
  expect(size_of(assets->video_path) > 0, "video file is non-empty");
  expect(size_of(assets->audio_path) > 0, "audio file is non-empty");
  std::printf("slate_pipeline_test: rendered %ju bytes video, %ju bytes audio\n",
              static_cast<std::uintmax_t>(size_of(assets->video_path)),
              static_cast<std::uintmax_t>(size_of(assets->audio_path)));

  // The render is only useful if what it wrote is what the looper will replay:
  // Annex-B AUs, one per frame, the first of them a keyframe (the loop restarts
  // there), and audio aacparse can read back.
  {
    std::ifstream in(assets->video_path, std::ios::binary);
    const std::vector<std::uint8_t> data {std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>()};
    const auto aus = standby::split_access_units(data.data(), data.size());
    expect(aus.size() == static_cast<std::size_t>(2 * req.fps),
           "the video ES has exactly 2*fps access units");
    expect(aus.front().first == 0
               && aus.back().first + aus.back().second == data.size(),
           "the access units tile the file");
    expect(standby::au_is_keyframe(standby::es_codec::h264,
                                   data.data() + aus.front().first,
                                   aus.front().second),
           "the first AU is an IDR: the loop restarts there");
    // The slate is joined wherever the loop happens to be when standby is
    // pressed, so every AU has to stand on its own, not just the first.
    std::size_t non_keyframes = 0;
    for (const auto& span : aus) {
      if (!standby::au_is_keyframe(standby::es_codec::h264,
                                   data.data() + span.first, span.second)) {
        ++non_keyframes;
      }
    }
    expect(non_keyframes == 0,
           std::format("every slate AU is an IDR ({} of {} are not)",
                       non_keyframes, aus.size()).c_str());
  }

  // Audio must parse as ADTS frames -- a file aacparse cannot read is a slate
  // with no audio branch at all.
  {
    GError* error = nullptr;
    const std::string desc =
        "filesrc location=\"" + assets->audio_path + "\" ! aacparse ! fakesink";
    GstElement* pipeline = gst_parse_launch(desc.c_str(), &error);
    if (error != nullptr) {
      g_error_free(error);
      error = nullptr;
    }
    expect(pipeline != nullptr, "the audio parse pipeline builds");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    expect(msg != nullptr, "the audio file reaches EOS");
    bool is_eos = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (!is_eos) {
      GError* gerror = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &gerror, &debug);
      std::fprintf(stderr, "FAIL: audio did not parse: %s\n",
                   gerror != nullptr ? gerror->message : "?");
      if (gerror != nullptr) {
        g_error_free(gerror);
      }
      g_free(debug);
    }
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    expect(is_eos, "the rendered audio is ADTS aacparse accepts");
  }

  // Second call is a cache hit: same paths, and the timestamp does not move,
  // which is what makes the slate free on a session restart.
  const auto mtime = std::filesystem::last_write_time(assets->video_path);
  const auto again = standby::render_slate(req, err);
  expect(again.has_value(), "a second render succeeds");
  expect(again->video_path == assets->video_path, "the cached video is reused");
  expect(std::filesystem::last_write_time(again->video_path) == mtime,
         "the cached video is not re-rendered");

  std::filesystem::remove_all(dir);
}

}  // namespace

auto main() -> int
{
  gst_init(nullptr, nullptr);
  test_plan();
  test_render();
  std::puts("slate_pipeline_test: ALL OK");
  return 0;
}
