// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Standby slate renderer (plan B1). Two GStreamer pipelines, run once per
// session at /start, producing a 2-second loop of pre-encoded elementary
// stream in the session's ingest codec plus silent AAC-LC.
//
// Why a one-shot render instead of a live pattern src: the slate has to enter
// the shared stage with the *same caps* the live branch carries, and it has to
// survive for as long as the session is held. Encoding it once means the
// runtime path is a memcpy loop (es_looper) with no encoder that can fail, and
// a restart of the stage costs nothing extra.
//
// Everything here is bounded and non-throwing: it runs on the /start path,
// where a broken VA driver or a missing font must degrade the slate (the
// session still starts, `standby.degraded`) and never hang the control plane.

#include "standby/render.h"

#include <glib.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace standby
{
namespace
{
// The render budget. Two seconds of a static picture at any preset is a few
// hundred ms; five seconds is the point at which we stop believing the encoder
// will ever finish and degrade the slate instead of holding /start open.
constexpr auto k_render_deadline = std::chrono::milliseconds {5000};

auto escape_pipeline_value(std::string_view value) -> std::string
{
  // gst_parse_launch takes a quoted value as-is; a path or a caption that
  // carries a quote or a backslash would otherwise end the value early and
  // either fail the parse or inject a property.
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

auto element_is_present(const char* name) -> bool
{
  GstElementFactory* factory = gst_element_factory_find(name);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

// The encoder fragment. Bitrate units differ by element: the VA encoders and
// x264/x265 take kbit/s, aom's av1enc calls the same quantity target-bitrate.
// The software encoders are pinned to their fastest preset because this runs
// inside the control plane's /start latency budget, and quality of a static
// slate is not what anyone is looking at.
//
// The slate is encoded all-intra: every frame is a keyframe. The loop replays
// the file from its first access unit, and the switch presents the branch at
// whatever point the loop has reached when an operator presses standby, so
// every access unit must be independently decodable. Anything short of
// all-intra leaves a mid-loop start showing a picture whose reference frames
// were never sent. A still image costs almost nothing at this content, and the
// output is a slate, not a programme.
auto encoder_fragment(const std::string& encoder, int bitrate_kbps)
    -> std::string
{
  if (encoder == "av1enc") {
    return std::format(
        "av1enc target-bitrate={} cpu-used=8 keyframe-max-dist=1",
        bitrate_kbps);
  }
  if (encoder == "x264enc" || encoder == "x265enc") {
    // CONSTANT QUALITY, not a rate cap. Every slate frame is a keyframe, so a
    // bitrate target divides into a per-frame budget that a 1080p intra frame
    // cannot fit: at the 1500 kbps default that is ~6 KB a frame, and the
    // encoder spends it and emits a flat grey card. Measured: a /start with no
    // bitrate_kbps rendered 46 of 60 frames blank, while the same body at
    // 2500 kbps rendered all 60. The slate is a two-second still looped
    // forever, so its size does not matter and its quality does.
    // `fast` + `stillimage`, not `ultrafast` + `zerolatency`. The slate is one
    // still rendered once, so latency does not matter and detail does: ultrafast
    // disables CABAC and the deblocking filter, which is what makes the caption
    // and the mark look blocky on a 2.5 Mbps stream. Measured on 1080p: fast is
    // pixel-identical to medium here and renders the whole 60-frame slate in
    // 0.34 s, so a small node still lands well inside the 5 s render deadline.
    (void)bitrate_kbps;
    return std::format(
        "{} pass=qual quantizer=14 speed-preset=fast tune=stillimage "
        "key-int-max=1",
        encoder);
  }
  if (encoder == "vah264enc" || encoder == "vah265enc"
      || encoder == "vaav1enc") {
    return std::format("{} bitrate={} key-int-max=1", encoder, bitrate_kbps);
  }
  // An encoder without a known GOP property: the looper reports the ES as not
  // all-intra, which is the honest state rather than a silent mid-GOP join.
  return std::format("{} bitrate={}", encoder, bitrate_kbps);
}

// One pipeline, synchronously, with a hard deadline. EOS is success; an ERROR
// message becomes "element: message"; a timeout becomes the element that never
// finished (the sink, in practice, so the message names the file).
auto run_pipeline(const std::string& desc, std::string& err) -> bool
{
  gst_init(nullptr, nullptr);

  GError* parse_error = nullptr;
  GstElement* pipeline = gst_parse_launch(desc.c_str(), &parse_error);
  std::string parse_message;
  if (parse_error != nullptr) {
    parse_message = parse_error->message;
    g_error_free(parse_error);
  }
  if (pipeline == nullptr) {
    err = "pipeline parse failed: " + parse_message;
    return false;
  }
  if (!parse_message.empty()) {
    // A recoverable parse warning (an unknown property, say) still lets the
    // pipeline exist; treat it as a failure because a slate built with a
    // dropped property is not the slate the caller asked for.
    gst_object_unref(pipeline);
    err = "pipeline parse failed: " + parse_message;
    return false;
  }

  bool ok = false;
  const auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    err = "pipeline refused to start";
  } else {
    const auto deadline =
        std::chrono::steady_clock::now() + k_render_deadline;
    GstBus* bus = gst_element_get_bus(pipeline);
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        err = "timed out waiting for EOS";
        break;
      }
      const auto remaining_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
      GstMessage* msg = gst_bus_timed_pop_filtered(
          bus, static_cast<GstClockTime>(remaining_ns.count()),
          static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
      if (msg == nullptr) {
        err = "timed out waiting for EOS";
        break;
      }
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* gerror = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &gerror, &debug);
        const GstObject* src = GST_MESSAGE_SRC(msg);
        err = std::format("{}: {}", src != nullptr ? GST_OBJECT_NAME(src) : "pipeline",
                          gerror != nullptr ? gerror->message : "error");
        if (gerror != nullptr) {
          g_error_free(gerror);
        }
        g_free(debug);
        gst_message_unref(msg);
        break;
      }
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        ok = true;
        gst_message_unref(msg);
        break;
      }
      gst_message_unref(msg);
    }
    gst_object_unref(bus);
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return ok;
}

auto non_empty_file(const std::string& path) -> bool
{
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec)
      && std::filesystem::file_size(path, ec) > 0;
}

auto remove_quietly(const std::string& path) -> void
{
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
}  // namespace

auto parse_slate_codec(std::string_view name, slate_codec& out) -> bool
{
  if (name == "h264") {
    out = slate_codec::h264;
    return true;
  }
  if (name == "h265") {
    out = slate_codec::h265;
    return true;
  }
  if (name == "av1") {
    out = slate_codec::av1;
    return true;
  }
  return false;
}

auto slate_codec_name(slate_codec cod) -> const char*
{
  switch (cod) {
    case slate_codec::h264:
      return "h264";
    case slate_codec::h265:
      return "h265";
    case slate_codec::av1:
      return "av1";
  }
  return "h264";
}

auto slate_encoder_alternatives(slate_codec cod) -> std::vector<std::string>
{
  // SOFTWARE ONLY, on every node. The slate is a two-second still rendered once
  // at /start and then only replayed, so a hardware encoder buys nothing here —
  // and it costs correctness: the VA elements ship in every image, so plugin
  // presence says nothing about the host. A cloud node with only /dev/dri/card0
  // selected vah264enc, encoded 60 access units, and 43 of them were a uniform
  // grey card, so the slate reached the destination as a flicker while every log
  // line said the slate was ready. One deterministic renderer for the whole fleet
  // removes the entire class, and the CPU cost is 60 ultrafast frames per session.
  switch (cod) {
    case slate_codec::h264:
      return {"x264enc"};
    case slate_codec::h265:
      return {"x265enc"};
    case slate_codec::av1:
      return {"av1enc"};
  }
  return {};
}

auto slate_parser(slate_codec cod) -> const char*
{
  switch (cod) {
    case slate_codec::h264:
      return "h264parse";
    case slate_codec::h265:
      return "h265parse";
    case slate_codec::av1:
      return "av1parse";
  }
  return "h264parse";
}

auto slate_extension(slate_codec cod) -> const char*
{
  switch (cod) {
    case slate_codec::h264:
      return "h264";
    case slate_codec::h265:
      return "h265";
    case slate_codec::av1:
      return "av1";
  }
  return "h264";
}

auto slate_parse_caps(slate_codec cod) -> const char*
{
  switch (cod) {
    case slate_codec::h264:
      return "video/x-h264,stream-format=byte-stream,alignment=au";
    case slate_codec::h265:
      return "video/x-h265,stream-format=byte-stream,alignment=au";
    case slate_codec::av1:
      // obu-stream at frame alignment is what mpegtsmux and tsdemux carry in
      // this stack, so the loop's access units are whole frames and match the
      // live branch's caps exactly (invariant: caps equality at the switch).
      return "video/x-av1,stream-format=obu-stream,alignment=frame";
  }
  return "";
}

auto slate_cache_key(const slate_request& req) -> std::string
{
  const std::string identity = std::format(
      "{}|{}|{}|{}|{}|{}|{}|all-intra", req.codec, req.width, req.height,
      req.fps, req.logo_path, req.brand_path, req.text);
  gchar* digest = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, identity.c_str(), static_cast<gssize>(identity.size()));
  std::string key {digest != nullptr ? digest : ""};
  g_free(digest);
  // 16 hex chars of sha256, as specified: a session-scoped cache directory
  // holds a handful of entries, so collision resistance beyond this buys
  // nothing.
  return key.substr(0, 16);
}

// The pixel size of an overlay asset, read from its header. gdkpixbufoverlay
// draws an image at its native size unless overlay-width/height are set, so a
// 1080-square brand on a small canvas covered the frame. Only the two formats
// the slate ships are handled; an unknown file is reported as absent and the
// overlay keeps its native size rather than being distorted.
bool image_pixel_size(const std::string& path, int& width, int& height)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  unsigned char head[32] {};
  in.read(reinterpret_cast<char*>(head), sizeof head);
  const auto got = in.gcount();
  if (got < 24) {
    return false;
  }
  // PNG: 8-byte signature, then IHDR length/type, then two big-endian u32.
  if (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') {
    auto be32 = [](const unsigned char* p) {
      return (static_cast<int>(p[0]) << 24) | (static_cast<int>(p[1]) << 16)
          | (static_cast<int>(p[2]) << 8) | static_cast<int>(p[3]);
    };
    width = be32(head + 16);
    height = be32(head + 20);
    return width > 0 && height > 0;
  }
  // JPEG: walk the marker chain to a start-of-frame segment.
  if (head[0] == 0xFF && head[1] == 0xD8) {
    in.clear();
    in.seekg(2);
    while (in) {
      unsigned char marker[2] {};
      in.read(reinterpret_cast<char*>(marker), 2);
      if (!in || marker[0] != 0xFF) {
        break;
      }
      const unsigned char id = marker[1];
      if (id == 0xD8 || id == 0x01 || (id >= 0xD0 && id <= 0xD7)) {
        continue;
      }
      unsigned char len_bytes[2] {};
      in.read(reinterpret_cast<char*>(len_bytes), 2);
      if (!in) {
        break;
      }
      const int len = (static_cast<int>(len_bytes[0]) << 8) | len_bytes[1];
      if (len < 2) {
        break;
      }
      const bool sof = id >= 0xC0 && id <= 0xCF && id != 0xC4 && id != 0xC8
          && id != 0xCC;
      if (sof) {
        unsigned char sof_head[5] {};
        in.read(reinterpret_cast<char*>(sof_head), sizeof sof_head);
        if (in) {
          height = (static_cast<int>(sof_head[0]) << 8) | sof_head[1];
          width = (static_cast<int>(sof_head[2]) << 8) | sof_head[3];
          return width > 0 && height > 0;
        }
        break;
      }
      in.seekg(len - 2, std::ios::cur);
    }
  }
  return false;
}

// An overlay scaled to fit a box, aspect preserved. Returns an empty string when
// the size is unknown, which leaves the element at its native size.
std::string scaled_overlay(const std::string& path, int offset_x, int offset_y,
                           int box_width, int box_height)
{
  int width = 0;
  int height = 0;
  std::string sizing;
  if (image_pixel_size(path, width, height)) {
    const double fit = std::min(static_cast<double>(box_width) / width,
                                static_cast<double>(box_height) / height);
    const int scaled_w = std::max(1, static_cast<int>(width * fit));
    const int scaled_h = std::max(1, static_cast<int>(height * fit));
    sizing = std::format(" overlay-width={} overlay-height={}", scaled_w,
                         scaled_h);
  }
  return std::format(
      "gdkpixbufoverlay location=\"{}\" offset-x={} offset-y={}{} ! ",
      escape_pipeline_value(path), offset_x, offset_y, sizing);
}


  
auto plan_slate(const slate_request& req,
                const slate_element_present_fn& present, std::string& err)
    -> std::optional<slate_plan>
{
  slate_codec cod {};
  if (!parse_slate_codec(req.codec, cod)) {
    err = std::format("unknown slate codec '{}'", req.codec);
    return std::nullopt;
  }
  if (req.width <= 0 || req.height <= 0 || req.fps <= 0) {
    err = std::format("slate geometry must be positive ({}x{}@{})", req.width,
                      req.height, req.fps);
    return std::nullopt;
  }
  if (req.out_dir.empty()) {
    err = "slate out_dir is empty";
    return std::nullopt;
  }

  slate_plan plan;
  plan.key = slate_cache_key(req);
  plan.video_path =
      std::format("{}/{}.{}", req.out_dir, plan.key, slate_extension(cod));
  plan.audio_path = std::format("{}/{}.aac", req.out_dir, plan.key);

  // Encoder choice. A pinned encoder is honoured even if the probe would have
  // picked another one; a pinned encoder that is absent is an error, not a
  // silent fallback, so a test or an operator learns the call was ignored.
  if (!req.encoder_override.empty()) {
    if (present != nullptr && !present(req.encoder_override.c_str())) {
      err = std::format("slate encoder '{}' is not installed",
                        req.encoder_override);
      return std::nullopt;
    }
    if (req.encoder_override.starts_with("va")
        && req.encoder_override.ends_with("enc")) {
      // Slate generation is software on every node (see
      // slate_encoder_alternatives). A hardware pin is refused here rather than
      // honoured: the failure mode is a slate that encodes cleanly and arrives
      // as a grey flicker, which no exit status reports.
      err = std::format(
          "slate encoder '{}' is not supported: the slate is rendered in "
          "software on every node",
          req.encoder_override);
      return std::nullopt;
    }
    plan.encoder = req.encoder_override;
  } else {
    std::string tried;
    for (const auto& candidate : slate_encoder_alternatives(cod)) {
      if (!tried.empty()) {
        tried += ", ";
      }
      tried += candidate;
      if (present != nullptr && present(candidate.c_str())) {
        plan.encoder = candidate;
        break;
      }
    }
    if (plan.encoder.empty()) {
      err = std::format("no {} encoder installed (tried {})",
                        slate_codec_name(cod), tried);
      return std::nullopt;
    }
  }

// The brand image is optional by design: a session may have no branding, and
  // a path that was configured but not uploaded must not cost the session its
  // slate. Anything that is not a readable regular file is treated as absent.
  if (!req.brand_path.empty()) {
    std::error_code ec;
    plan.brand_overlay = std::filesystem::is_regular_file(req.brand_path, ec);
  }

  const std::string parser = slate_parser(cod);
  // config-interval=-1 repeats SPS/PPS on every keyframe for h264/h265; av1parse
  // exposes no such property (its sequence header is in-band in the TU).
  const std::string parser_props = cod == slate_codec::av1
      ? parser
      : std::format("{} config-interval=-1", parser);

  // The logo is bottom/top-left at a fixed 40 px inset; the brand image sits
  // 40 px from the right edge. gdkpixbufoverlay reads a negative offset-x as
  // "from the right", so the layout does not need the image's pixel width.
  std::string overlays;
  // The inset is a fixed 40 px, as every slate before this one drew it; the
  // assets and the caption scale, the margin does not. The box is a sixth of
  // the frame: the slate is branding, not a picture.
  constexpr int inset = 40;
  const int box_w = std::max(16, req.width / 6);
  const int box_h = std::max(16, req.height / 6);
  if (!req.logo_path.empty()) {
    overlays += scaled_overlay(req.logo_path, inset, inset, box_w, box_h);
  }
  if (plan.brand_overlay) {
    overlays += scaled_overlay(req.brand_path, -inset, inset, box_w, box_h);
  }

  // videoconvert + NV12 is not cosmetic: the VA encoders' only system-memory
  // sink format is NV12 (vah264enc rejects I420 outright) while the overlays
  // work best on the packed formats, so the conversion is stated rather than
  // left to negotiation.
  plan.video_pipeline = std::format(
      "videotestsrc pattern=black num-buffers={} ! "
      "video/x-raw,width={},height={},framerate={}/1,format=I420 ! "
      "{}"
      "textoverlay text=\"{}\" font-desc=\"Sans 48\" halignment=center "
      "valignment=bottom ! "
      "videoconvert ! video/x-raw,format=NV12 ! "
      "{} ! {} ! {} ! filesink location=\"{}\"",
      2 * req.fps, req.width, req.height, req.fps, overlays,
      escape_pipeline_value(req.text),
      encoder_fragment(plan.encoder, req.bitrate_kbps),
      parser_props, slate_parse_caps(cod),
      escape_pipeline_value(plan.video_path));

  // Silence, not "nothing": the audio branch of the shared stage must stay
  // negotiated for the whole session, or the muxer drops its audio pad and a
  // destination reconnects at the first live sample.
  //
  // The buffer size is stated because num-buffers is a *count*: at the default
  // 1024 samples a "2*fps" buffer file is 1.28 s at 30 fps, not 2 s, and the
  // audio would then loop out of step with the video for the life of the
  // session. 48000/fps samples per buffer makes the count a duration, and the
  // AAC encoder re-frames it to whole 1024-sample packets regardless.
  //
  // The ADTS caps are stated for the same reason: aacparse's src format is
  // negotiated, and with nothing downstream to ask, 1.28 here writes a
  // raw-stream file ("Lavc..." tag and all) that aacparse then refuses to read
  // back -- a slate whose audio branch could never be looped or re-parsed.
  const int audio_samples_per_buffer = std::max(1, 48000 / req.fps);
  plan.audio_pipeline = std::format(
      "audiotestsrc wave=silence num-buffers={} samplesperbuffer={} ! "
      "audio/x-raw,rate=48000,channels=2 ! "
      "avenc_aac bitrate=128000 ! aacparse ! "
      "audio/mpeg,stream-format=adts,parsed=true ! "
      "filesink location=\"{}\"",
      2 * req.fps, audio_samples_per_buffer,
      escape_pipeline_value(plan.audio_path));

  return plan;
}

auto render_slate(const slate_request& req, std::string& err)
    -> std::optional<slate_assets>
{
  // Before anything probes the registry: gst_element_factory_find finds nothing
  // until GStreamer is initialised, and a caller that never touched the library
  // (a test, a probe) would otherwise be told no encoder is installed. Safe to
  // call repeatedly; main.cpp has usually already done it.
  gst_init(nullptr, nullptr);

  std::string plan_err;
  const auto plan = plan_slate(req, &element_is_present, plan_err);
  if (!plan.has_value()) {
    err = plan_err;
    return std::nullopt;
  }

  // Cache hit: both halves must be present and non-empty. Half a slate is not
  // reusable -- a session that lost only the audio file would otherwise push
  // video into an unnegotiated audio branch for its whole life.
  if (non_empty_file(plan->video_path) && non_empty_file(plan->audio_path)) {
    err.clear();
    return slate_assets {plan->video_path, plan->audio_path};
  }

  std::error_code ec;
  std::filesystem::create_directories(req.out_dir, ec);
  if (ec) {
    err = std::format("cannot create slate directory '{}': {}", req.out_dir,
                      ec.message());
    return std::nullopt;
  }

  if (!run_pipeline(plan->video_pipeline, err)) {
    remove_quietly(plan->video_path);
    err = std::format("slate video render failed ({})", err);
    return std::nullopt;
  }
  if (!run_pipeline(plan->audio_pipeline, err)) {
    remove_quietly(plan->audio_path);
    err = std::format("slate audio render failed ({})", err);
    return std::nullopt;
  }

  if (!non_empty_file(plan->video_path) || !non_empty_file(plan->audio_path)) {
    err = "slate render produced an empty file";
    remove_quietly(plan->video_path);
    remove_quietly(plan->audio_path);
    return std::nullopt;
  }

  err.clear();
  return slate_assets {plan->video_path, plan->audio_path};
}

}  // namespace standby
