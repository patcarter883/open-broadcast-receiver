// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

namespace
{
constexpr std::chrono::milliseconds k_read_timeout {100};
constexpr std::size_t k_read_chunk = 32 * 1024;
constexpr std::chrono::seconds k_backoff_initial {1};
constexpr std::chrono::seconds k_backoff_cap {30};
// A connection that survives this long resets the backoff to initial — a
// mid-event platform blip after hours of streaming restarts fast again.
constexpr std::chrono::seconds k_stable_after {30};
// The transcode video appsrc is capped in FRAMES, not bytes: a fixed byte cap
// is a different amount of slack at every resolution (4 MB is 2.9 frames of
// 720p NV12 and 7.7 of 360p), so one number cannot mean "a few frames ahead of
// the encoder" for both. Eight frames holds ~0.27 s of 720p, enough for the
// feed to stay ahead of an encoder running at ingest rate without adding
// meaningful latency; the floor keeps small frames from being capped tighter
// than the element's own default.
constexpr guint64 k_appsrc_frame_span = 8;
constexpr guint64 k_appsrc_default_bytes = 4 * 1024 * 1024;
constexpr guint64 k_appsrc_ceiling_bytes = 64 * 1024 * 1024;
// How long the transit buffer may stay pinned at its limit before the sink is
// treated as stalled and the attempt is rebuilt.
constexpr auto k_stall_timeout = std::chrono::seconds(5);
// Period of the transcode feed's accounting line.
constexpr auto k_feed_stats_period = std::chrono::seconds(2);

auto now_ms() -> int64_t
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// F4 defence-in-depth: strip the stream key / SRT streamid from any text
// before it is logged or exposed via /stats last_error. The key is set as a
// pipeline PROPERTY (never in a URI), so sink error text should not contain it
// — but redact regardless so a future GStreamer element/version can't regress
// the "the key is never logged" invariant (lib.h/CONTRACT §2/§9). Guarded on a
// sane secret length so a short/empty key can't blank unrelated text.
auto redact_secret(std::string text, const std::string& secret) -> std::string
{
  if (secret.size() < 4) {
    return text;
  }
  constexpr std::string_view k_mask = "[REDACTED]";
  for (std::string::size_type pos = text.find(secret); pos != std::string::npos;
       pos = text.find(secret, pos + k_mask.size())) {
    text.replace(pos, secret.size(), k_mask);
  }
  return text;
}

// Elements each template needs (TRANSPORT_PROFILE §1.2 element_unavailable)
// now live in lib (required_elements) so the preflight is unit-testable
// without GStreamer; see also registry_has_element below.

auto has_property(GstElement* elem, const char* name) -> bool
{
  return g_object_class_find_property(G_OBJECT_GET_CLASS(elem), name)
      != nullptr;
}

// Real registry probe for the element preflight: exact factory existence.
auto registry_has_element(const char* name) -> bool
{
  GstElementFactory* factory = gst_element_factory_find(name);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

auto count_buffer_probe(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer data)
    -> GstPadProbeReturn
{
  static_cast<std::atomic<uint64_t>*>(data)->fetch_add(
      1, std::memory_order_relaxed);
  return GST_PAD_PROBE_PASS;
}

}  // namespace

output::output(output_config cfg,
               ts_ring& ring,
               frame_ring* frames,
               log_fn log,
               repin_fn repin)
    : m_cfg {std::move(cfg)}
    , m_ring {ring}
    , m_frames {frames}
    , m_log {std::move(log)}
    , m_repin {std::move(repin)}
{
}

output::~output()
{
  stop();
}

auto output::log(const std::string& msg) const -> void
{
  if (m_log) {
    // Per CONTRACT §2/§9 nothing here may contain the stream key; callers
    // only pass template/host/state text.
    m_log("[output " + m_cfg.id + "] " + msg);
  }
}

auto output::state_name() const -> const char*
{
  switch (m_state.load(std::memory_order_acquire)) {
    case run_state::starting:
      return "starting";
    case run_state::running:
      return "running";
    case run_state::reconnecting:
      return "reconnecting";
    case run_state::error:
      return "error";
    case run_state::stopped:
      return "stopped";
  }
  return "stopped";
}

auto output::connected_s() const -> int64_t
{
  const int64_t since = m_running_since_ms.load(std::memory_order_relaxed);
  if (since == 0
      || m_state.load(std::memory_order_acquire) != run_state::running)
  {
    return 0;
  }
  return (now_ms() - since) / 1000;
}

auto output::last_error() const -> std::string
{
  std::lock_guard<std::mutex> guard(m_err_mutex);
  return m_last_error;
}

auto output::pipeline() const -> GstElement*
{
  std::lock_guard<std::mutex> guard(m_pipe_mutex);
  if (m_pipeline == nullptr) {
    return nullptr;
  }
  return GST_ELEMENT(gst_object_ref(m_pipeline));
}

auto output::retarget_bitrate(int measured_link_kbps) -> void
{
  if (m_cfg.transcode.target == transcode_target::none) {
    return;  // copy-only: no encoder to re-target
  }
  if (measured_link_kbps < 0) {
    measured_link_kbps = 0;
  }
  m_measured_link_kbps.store(measured_link_kbps, std::memory_order_relaxed);

  const int target = derive_transcode_bitrate_kbps(
      measured_link_kbps, m_cfg.transcode.bitrate_kbps);
  if (target <= 0) {
    return;
  }
  const int applied = m_applied_bitrate_kbps.load(std::memory_order_relaxed);
  if (!bitrate_change_is_material(applied, target)) {
    return;
  }

  // The pipeline belongs to the worker thread; take a reference under the same
  // mutex pipeline() uses, then touch the encoder outside it. If no attempt is
  // live, the value is picked up by the next build's target computation.
  GstElement* pipe = nullptr;
  {
    std::lock_guard<std::mutex> guard(m_pipe_mutex);
    if (m_pipeline != nullptr) {
      pipe = GST_ELEMENT(gst_object_ref(m_pipeline));
    }
  }
  if (pipe != nullptr) {
    GstElement* venc = gst_bin_get_by_name(GST_BIN(pipe), "venc");
    if (venc != nullptr) {
      if (has_property(venc, "bitrate")) {
        g_object_set(venc, "bitrate", static_cast<gint>(target), nullptr);
        m_applied_bitrate_kbps.store(target, std::memory_order_relaxed);
        log(std::format(
            "bitrate {} -> {} kbps (link measured {} kbps, portal {} kbps)\n",
            applied, target, measured_link_kbps,
            m_cfg.transcode.bitrate_kbps));
      }
      gst_object_unref(venc);
    }
    gst_object_unref(pipe);
  }
}

auto output::set_last_error(const std::string& err) -> void
{
  std::lock_guard<std::mutex> guard(m_err_mutex);
  m_last_error = err;
}

auto output::encoder_name() const -> std::string
{
  std::lock_guard<std::mutex> guard(m_enc_mutex);
  return m_encoder_name;
}

auto output::set_state(run_state next) -> void
{
  m_state.store(next, std::memory_order_release);
  if (next == run_state::running) {
    m_running_since_ms.store(now_ms(), std::memory_order_relaxed);
  } else {
    m_running_since_ms.store(0, std::memory_order_relaxed);
  }
}

auto output::first_missing_element(output_proto proto,
                                   const transcode_config& transcode,
                                   codec in_codec)
    -> std::optional<missing_requirement>
{
  return first_missing_requirement(
      required_elements(proto, transcode, in_codec), &registry_has_element);
}

// ---------------------------------------------------------------------------
// Pipeline construction (TRANSPORT_PROFILE §3.5)
// ---------------------------------------------------------------------------

auto output_template(output_proto proto,
                     const transcode_config& transcode,
                     const char* encoder) -> std::string
{
  // block=false on purpose: with block=true a sink that stops draining parks
  // the feed thread INSIDE push_buffer forever, so the stall/audio checks below
  // never run and nothing ever reconnects. Dropping under pressure is the
  // correct behaviour for a live output.
  constexpr std::string_view k_appsrc =
      "appsrc name=osrc is-live=true do-timestamp=true format=time "
      "block=false max-bytes=4194304 ";
  // caps=video/mpegts,systemstream=true is MANDATORY here: this appsrc feeds a
  // tsparse, and tsparse will not link to an uncaps'd source. Without it the
  // audio branch dies silently, the muxer stays capless on its audio pad and
  // never emits, so the sink never prerolls and the publish looks frozen.
  constexpr std::string_view k_audio_appsrc =
      "appsrc name=asrc is-live=true do-timestamp=true format=time "
      "block=false max-bytes=4194304 caps=video/mpegts,systemstream=true ";

  if (transcode.target == transcode_target::none) {
    // Copy-only templates — byte-identical to the pre-transcode tier. An rtmp
    // copy keeps legacy flvmux; only h265 transcode opts into eflvmux.
    switch (proto) {
      case output_proto::srt:
        return std::string {k_appsrc}
            + "! tsparse alignment=7 "
              "! srtsink name=osink wait-for-connection=false";
      case output_proto::rist:
        return std::string {k_appsrc}
            + "! tsparse alignment=7 ! ristsink name=osink";
      case output_proto::rtmp:
      case output_proto::rtmps:
        // The demuxer is left unchained: a `d.` link binds whichever pad
        // tsdemux announces first, and a TS may announce its audio pad first.
        // The branches are named and demux_pad_added links each pad to the
        // branch its caps belong to: video/x-h264 -> vq, audio/mpeg -> aq,
        // everything else -> the ddrop funnel.
        //
        // The audio branch ends at `acaps` and is attached to the muxer only
        // when the source TS exposes an audio pad, so flvmux never holds a
        // capless audio pad and video publishes on the first frame.
        return std::string {k_appsrc}
            + "! tsparse set-timestamps=true alignment=7 "
              "! tsdemux name=d "
              "queue name=vq ! h264parse config-interval=-1 "
              "! video/x-h264,stream-format=avc,alignment=au ! mux. "
              "queue name=aq ! aacparse ! capsfilter name=acaps "
              "caps=audio/mpeg,mpegversion=4,stream-format=raw "
              "flvmux name=mux streamable=true latency=1000000000 "
              "! rtmp2sink name=osink async-connect=true "
              "funnel name=ddrop ! fakesink sync=false async=false";
    }
    return {};
  }

  // Transcode targets are h264, h265 and av1. av1 is representable, but only over
  // the MPEG-TS carriers: flvmux/eflvmux expose no video/x-av1 caps, so
  // validate_transcode_targets refuses av1 -> rtmp/rtmps at /start and it can
  // never reach the rtmp cases below.
  const std::string enc = encoder != nullptr ? encoder : "";
  const std::string parse = transcode_target_parser(transcode.target);

  // The encoder is handed 4:2:0 8-bit and nothing else. Every destination this
  // product serves is 4:2:0, the decode stage publishes NV12, and the two
  // encoders' sink caps are wide (x264enc lists Y444/I422/I420_10LE, vah264enc
  // lists the VA memory variants of the same set) -- so an unconstrained link
  // lets negotiation pick a 4:4:4 or 10-bit layout, and the muxer then carries a
  // High 4:4:4 10-bit stream that the destination cannot decode. The filter
  // states the format rather than converting it: a stream that does not satisfy
  // it fails to negotiate instead of silently changing profile. Placed after the
  // scaler so the scaler's own output is constrained by it too.
  constexpr std::string_view k_raw_nv12 = "video/x-raw,format=NV12 ! ";

  // An output size is applied to the RAW frames BEFORE the encoder, so the
  // destination receives the size it was promised rather than the ingest size.
  //
  // Scaling runs on the GPU (vapostproc), which accepts plain system-memory frames
  // and uploads internally -- so the frame ring stays a GStreamer-free byte arena
  // and needs no change. Measured on the 1080p-upscale this tier actually performs
  // (600 frames = 10 s of video, wall/video, below 1.0 is faster than realtime):
  //
  //                1080p->1440p   1080p->2160p
  //   CPU bilinear      0.71           1.13   <- fails realtime
  //   CPU lanczos       0.71           1.21   <- fails realtime
  //   GPU vapostproc    0.44           0.75
  //
  // Two conclusions. The CPU scaler CANNOT hold realtime when the ladder is climbed
  // to 2160p, so this is what makes that tier possible rather than an optimisation
  // of it. And the GPU is ~40% faster at 1440p too, where the CPU was only just
  // keeping ahead. vapostproc selects its own filtering, so the separate
  // videoscale method=lanczos choice disappears with it.
  //
  // The size is applied to frames on their way INTO the encoder, so a destination
  // receives the size it was promised rather than the ingest size.
  std::string scale;
  if (transcode.scale_width > 0 && transcode.scale_height > 0) {
    scale = std::format("vapostproc "
                        "! video/x-raw,width={},height={} ! ",
                        transcode.scale_width,
                        transcode.scale_height);
  }

  switch (proto) {
    case output_proto::rtmp:
    case output_proto::rtmps: {
      const bool hevc = transcode.target == transcode_target::h265;
      const char* mux = transcode_muxer(proto, transcode.target);
      // The profile is stated in caps, not a property: the VA encoders expose no
      // profile property at all (vah264enc's and vah265enc's GStreamer property
      // lists are identical and contain none), so caps are the only way. It has to
      // be stated because two defaults depend on it -- `cabac` is true and
      // "requires main profile at least", `dct8x8` is true and "requires high
      // profile at least" -- so with no profile requested, both defaults ran
      // against a profile that may not have permitted them. h264 asks for high
      // (the superset, and what YouTube accepts); h265 asks for main, the
      // interoperable choice for HEVC.
      const std::string vcaps = hevc
          ? "video/x-h265,stream-format=hvc1,alignment=au,profile=main"
          : "video/x-h264,stream-format=avc,alignment=au,profile=high";
      // Encoded video is driven from the shared frame ring; audio stays on the
      // ts_ring AAC copy path so RTMP audio passthrough is unchanged. h265
      // uses the Enhanced FLV muxer (eflvmux) — legacy flvmux cannot carry
      // H.265.
      //
      // Audio is declared, never discovered: the source is AAC-LC 48 kHz
      // stereo, so the branch states that and carries the matching
      // AudioSpecificConfig (0x11 0x90: AOT 2, 48 kHz, 2 channels) as
      // codec_data, which is what FLV requires and what flvmux otherwise
      // waits for. The branch ENDS at `acaps` in this template: the muxer's
      // audio pad is requested only from demux_pad_added(), i.e. only when the
      // source TS actually exposes an audio pad. A video-only ingest therefore
      // never hands flvmux an audio pad to wait on, and video publishes on the
      // first encoded frame with no timeout involved.
      return std::format(
          "{}! queue ! {}{}{} name=venc ! {} config-interval=-1 ! {} ! mux. "
          "{}! tsparse set-timestamps=true alignment=7 ! tsdemux name=ad "
          "queue name=aq ! aacparse ! capsfilter name=acaps "
          "caps=audio/mpeg,mpegversion=4,stream-format=raw,rate=48000,channels=2,"
          "codec_data=(buffer)1190 "
          "{} name=mux streamable=true "
          // The transcode sink does not sync to the pipeline clock. Its input is
          // a live drop-oldest ring: pacing the encoder by the output clock
          // makes every transient downstream delay a permanent deficit, because
          // the frames needed to catch up have already been dropped by the
          // ring. The media is live, so its pacing comes from ingest.
          "! rtmp2sink name=osink async-connect=true sync=false "
          "funnel name=ddrop ! fakesink sync=false async=false",
          k_appsrc,
          scale,
          k_raw_nv12,
          enc,
          parse,
          vcaps,
          k_audio_appsrc,
          mux);
    }
    case output_proto::srt: {
      // Re-encoded video to a fresh TS. AAC is carried only on the rtmp path;
      // the srt/rist transcode chain is video-only.
      // enable-custom-mappings is set ONLY for av1: mpegtsmux has no standard
      // stream_type for it ("custom mappings for which there are no official
      // specifications"), so without the flag the mux never negotiates. Setting
      // it for h264/h265 would change their stream_type mapping for no reason.
      const char* custom = transcode.target == transcode_target::av1
          ? " enable-custom-mappings=true" : "";
      return std::format(
          "{}! queue ! {}{}{} name=venc ! {} config-interval=-1 "
          "! mpegtsmux alignment=7{} "
          "! srtsink name=osink wait-for-connection=false",
          k_appsrc,
          scale,
          k_raw_nv12,
          enc,
          parse,
          custom);
    }
    case output_proto::rist: {
      const char* custom = transcode.target == transcode_target::av1
          ? " enable-custom-mappings=true" : "";
      return std::format(
          "{}! queue ! {}{}{} name=venc ! {} config-interval=-1 "
          "! mpegtsmux alignment=7{} ! ristsink name=osink",
          k_appsrc,
          scale,
          k_raw_nv12,
          enc,
          parse,
          custom);
    }
  }
  return {};
}

auto output::build_pipeline(std::string& err) -> bool
{
  // The parse string carries NO secrets and NO customer URLs — the sink is
  // created unconfigured (name=osink) and its connect properties are set via
  // g_object_set below. That keeps pipeline strings loggable by definition.
  const bool is_transcode =
      m_cfg.transcode.target != transcode_target::none;
  std::string chosen_encoder;
  if (is_transcode) {
    if (m_frames == nullptr) {
      err = "transcode output has no frame ring";
      return false;
    }
    const auto enc = choose_present(
        transcode_encoder_alternatives(m_cfg.transcode.target),
        &registry_has_element);
    if (!enc) {
      // Preflighted at /start; a race is treated as a retryable build failure.
      err = "no transcode encoder available";
      return false;
    }
    chosen_encoder = *enc;
  }

  const std::string tmpl = output_template(
      m_cfg.type,
      m_cfg.transcode,
      is_transcode ? chosen_encoder.c_str() : nullptr);

  GError* gerr = nullptr;
  GstElement* parsed = gst_parse_launch(tmpl.c_str(), &gerr);
  {
    std::lock_guard<std::mutex> guard(m_pipe_mutex);
    m_pipeline = parsed;
  }
  if (m_pipeline == nullptr || gerr != nullptr) {
    err = (gerr != nullptr && gerr->message != nullptr)
        ? gerr->message
        : "gst_parse_launch failed";
    if (gerr != nullptr) {
      g_error_free(gerr);
    }
    destroy_pipeline();
    return false;
  }

  m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "osrc");
  GstElement* sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "osink");
  if (m_appsrc == nullptr || sink == nullptr) {
    if (sink != nullptr) {
      gst_object_unref(sink);
    }
    err = "pipeline is missing osrc/osink";
    destroy_pipeline();
    return false;
  }

  gst_app_src_set_max_bytes(GST_APP_SRC(m_appsrc), k_appsrc_default_bytes);

  if (is_transcode) {
    // The feed thread waits on this mutex when the video appsrc is at its cap;
    // a buffer leaving the appsrc is the event that frees room, so the wait is
    // the pipeline's own pace rather than a timer.
    GstPad* osrc_pad = gst_element_get_static_pad(m_appsrc, "src");
    if (osrc_pad != nullptr) {
      gst_pad_add_probe(osrc_pad, GST_PAD_PROBE_TYPE_BUFFER,
                        &output::appsrc_drained, this, nullptr);
      gst_object_unref(osrc_pad);
    }
    m_audio_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "asrc");
    if (m_audio_appsrc != nullptr) {
      gst_app_src_set_max_bytes(GST_APP_SRC(m_audio_appsrc),
                                k_appsrc_default_bytes);
    }
    // ---- Encoder tuning: the relay's own mechanics -------------------------
    // This is the RELAY's business, not policy: it is about making the hardware
    // encoder behave correctly for live fan-out. Bitrate and GOP stay the portal's
    // decision (the portal posts them; the relay only carries them).
    //
    // Every set is guarded by has_property, because the three hardware encoders
    // differ -- vaav1enc has no b-frames property, for instance -- and an
    // unguarded set would abort the pipeline for one target while working for
    // another.
    GstElement* venc = gst_bin_get_by_name(GST_BIN(m_pipeline), "venc");
    if (venc != nullptr) {
      // The target this attempt starts with: the link-derived rate when the
      // controller has a measurement, else the portal's figure (0 = leave the
      // encoder's default alone). The controller re-targets it live as the
      // measurement moves; starting from the portal value would open every
      // session at a rate the link has never been observed to carry.
      const int target_kbps = derive_transcode_bitrate_kbps(
          m_measured_link_kbps.load(std::memory_order_relaxed),
          m_cfg.transcode.bitrate_kbps);

      // Rate control: CBR. This is also the element default, but it is set
      // explicitly because the whole point of the tier is a predictable stream per
      // destination, and a default is not a guarantee.
      if (has_property(venc, "rate-control")) {
        g_object_set(venc, "rate-control", 2 /* cbr */, nullptr);
      }
      // bitrate=0 means "auto-calculate", which the encoder resolves to a
      // near-lossless rate -- measured at ~440 Mbps for 720p, which starved the
      // other outputs of frames. A CBR encoder with no bitrate is not a
      // configuration, so refuse the silence: log it loudly and leave the encoder
      // alone rather than pretend a target was set.
      if (target_kbps != 0) {
        if (has_property(venc, "bitrate")) {
          g_object_set(venc,
                       "bitrate",
                       static_cast<gint>(target_kbps),
                       nullptr);
          m_applied_bitrate_kbps.store(target_kbps, std::memory_order_relaxed);
        }
      } else {
        m_applied_bitrate_kbps.store(0, std::memory_order_relaxed);
        log(std::format(
            "output {}: no measured link and no bitrate_kbps; the hardware "
            "encoder will auto-calculate a near-lossless rate\n",
            m_cfg.id));
      }
      // Live fan-out: no B-frames and no B-pyramid. Both add encode latency and
      // reordering delay for a stream whose only purpose is to be relayed now.
      if (has_property(venc, "b-frames")) {
        g_object_set(venc, "b-frames", 0, nullptr);
      }
      if (has_property(venc, "b-pyramid")) {
        g_object_set(venc, "b-pyramid", FALSE, nullptr);
      }
      // target-usage 1..7, higher is faster and lower is better quality. Set to
      // the vendor's balanced default (4) explicitly rather than inherited.
      //
      // Measured on this driver (amd VAAPI, RX 9070): 4 and 7 produce
      // byte-identical output in identical time, so the setting is inert here and
      // choosing 7 bought nothing. It is NOT inert on every VAAPI driver, so the
      // value is pinned deliberately rather than left to whatever the driver
      // happens to default to. Do not push it to 1: measured, it OVERRAN the
      // requested bitrate by 24% (37.2 Mbps against a 30 Mbps target), which on a
      // live uplink is exactly the overshoot the rate is there to prevent.
      //
      // Bitrate, not this knob, is what sets picture quality under CBR.
      if (has_property(venc, "target-usage")) {
        g_object_set(venc, "target-usage", 4, nullptr);
      }
      if (m_cfg.transcode.gop != 0 && has_property(venc, "key-int-max")) {
        g_object_set(venc,
                     "key-int-max",
                     static_cast<guint>(m_cfg.transcode.gop),
                     nullptr);
      }
      // Report what was ACTUALLY applied. The bitrate is the one setting whose
      // absence is invisible: the encoder just runs at its auto rate, the output
      // still looks healthy, and the only symptom is the byte rate -- which is
      // exactly how a silent miss here hides. The VA device is reported for the
      // same reason: libva takes the first render node, and on a multi-GPU host
      // naming the node is how an operator can tell which card is encoding.
      {
        gint applied_bitrate = -1;
        guint applied_gop = 0;
        if (has_property(venc, "bitrate")) {
          g_object_get(venc, "bitrate", &applied_bitrate, nullptr);
        }
        if (has_property(venc, "key-int-max")) {
          g_object_get(venc, "key-int-max", &applied_gop, nullptr);
        }
        gchar* va_device = nullptr;
        if (has_property(venc, "device-path")) {
          g_object_get(venc, "device-path", &va_device, nullptr);
        }
        log(std::format(
            "output {}: encoder {} bitrate={} kbps gop={} (target {}/gop {}, "
            "link {} kbps, portal {} kbps) hardware-required, va-device={}\n",
            m_cfg.id, chosen_encoder, applied_bitrate, applied_gop,
            target_kbps, m_cfg.transcode.gop,
            m_measured_link_kbps.load(std::memory_order_relaxed),
            m_cfg.transcode.bitrate_kbps,
            va_device != nullptr ? va_device : "n/a"));
        g_free(va_device);
      }
      gst_object_unref(venc);
    }
    {
      std::lock_guard<std::mutex> guard(m_enc_mutex);
      m_encoder_name = chosen_encoder;
    }
  }

  // ---- connect target (M1.2: the vetted, pinned IP — never re-resolve) ----
  switch (m_cfg.type) {
    case output_proto::srt: {
      const std::string uri =
          std::format("srt://{}:{}", m_cfg.pinned_ip, m_cfg.port);
      g_object_set(sink, "uri", uri.c_str(), nullptr);
      if (!m_cfg.key_or_streamid.empty()) {
        // streamid as a property, not in the URI: URIs are loggable.
        if (has_property(sink, "streamid")) {
          g_object_set(sink, "streamid", m_cfg.key_or_streamid.c_str(),
                       nullptr);
        } else {
          const std::string with_id = std::format(
              "srt://{}:{}?streamid={}",
              m_cfg.pinned_ip, m_cfg.port, m_cfg.key_or_streamid);
          g_object_set(sink, "uri", with_id.c_str(), nullptr);
        }
      }
      break;
    }
    case output_proto::rist: {
      g_object_set(sink,
                   "address", m_cfg.pinned_ip.c_str(),
                   "port", static_cast<guint>(m_cfg.port),
                   nullptr);
      break;
    }
    case output_proto::rtmp:
    case output_proto::rtmps: {
      // Plain rtmp connects to the pinned IP (tcUrl carries the IP — verified
      // against SRS/platform ingests in the M1.6 rig). rtmps keeps the
      // HOSTNAME: TLS certificate validation against the real hostname is
      // itself the rebinding defence for TLS destinations (an attacker's
      // private-IP endpoint cannot present a valid cert for the platform
      // host), and SNI/verification break on a bare IP. Recorded in
      // DECISIONS.md.
      const bool secure = m_cfg.type == output_proto::rtmps;
      const std::string connect_host =
          secure ? m_cfg.host : m_cfg.pinned_ip;
      std::string app = m_cfg.path;
      if (!app.empty() && app.front() == '/') {
        app.erase(0, 1);
      }
      gst_util_set_object_arg(G_OBJECT(sink), "scheme",
                              secure ? "rtmps" : "rtmp");
      g_object_set(sink,
                   "host", connect_host.c_str(),
                   "port", static_cast<guint>(m_cfg.port),
                   "application", app.c_str(),
                   "stream", m_cfg.key_or_streamid.c_str(),
                   nullptr);
      // Audio-degrade observation (TRANSPORT_PROFILE §1.2): a non-AAC audio
      // ES leaves the audio branch unlinked (video-only output) and reports
      // audio_dropped=true instead of failing the output. "d" is the copy
      // path's demux, "ad" the transcode path's audio-only demux.
      for (const char* demux_name : {"d", "ad"}) {
        GstElement* demux = gst_bin_get_by_name(GST_BIN(m_pipeline), demux_name);
        if (demux != nullptr) {
          g_signal_connect(demux, "pad-added",
                           G_CALLBACK(&output::demux_pad_added), this);
          gst_object_unref(demux);
        }
      }
      // Where the feed's frames go after the appsrc: buffers arriving at the
      // encoder and buffers the muxer emits. A pinned appsrc with a low encoder
      // input count is the encoder, a high input count with a low output count
      // is the muxer or the sink.
      if (GstElement* enc = gst_bin_get_by_name(GST_BIN(m_pipeline), "venc");
          enc != nullptr) {
        if (GstPad* pad = gst_element_get_static_pad(enc, "sink");
            pad != nullptr) {
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                            &count_buffer_probe, &m_encoder_in, nullptr);
          gst_object_unref(pad);
        }
        gst_object_unref(enc);
      }
      if (GstElement* muxer = gst_bin_get_by_name(GST_BIN(m_pipeline), "mux");
          muxer != nullptr) {
        if (GstPad* pad = gst_element_get_static_pad(muxer, "src");
            pad != nullptr) {
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                            &count_buffer_probe, &m_muxer_out, nullptr);
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                            &output::muxer_first_buffer, this, nullptr);
          gst_object_unref(pad);
        }
        gst_object_unref(muxer);
      }
      break;
    }
  }
  gst_object_unref(sink);

  m_bus = gst_element_get_bus(m_pipeline);
  return true;
}

auto output::link_pad_to(GstPad* pad,
                         const char* element_name,
                         const char* pad_name) -> bool
{
  if (m_pipeline == nullptr) {
    return false;
  }
  GstElement* element = gst_bin_get_by_name(GST_BIN(m_pipeline), element_name);
  if (element == nullptr) {
    return false;
  }
  // A requested pad (funnel's sink_%u) takes one stream per request; a static
  // pad is a single branch and a second stream of the same kind goes elsewhere.
  GstPad* sink = g_str_has_suffix(pad_name, "%u")
      ? gst_element_request_pad_simple(element, pad_name)
      : gst_element_get_static_pad(element, pad_name);
  bool linked = false;
  if (sink == nullptr) {
    log(std::format("{} has no {} pad\n", element_name, pad_name));
  } else if (gst_pad_is_linked(sink)) {
    log(std::format("{} already carries another demux pad\n", element_name));
  } else {
    const GstPadLinkReturn lr = gst_pad_link(pad, sink);
    linked = lr == GST_PAD_LINK_OK;
    if (!linked) {
      log(std::format("demux pad {} could not join {} ({})\n",
                      GST_PAD_NAME(pad) != nullptr ? GST_PAD_NAME(pad) : "?",
                      element_name, gst_pad_link_get_name(lr)));
    }
  }
  if (sink != nullptr) {
    gst_object_unref(sink);
  }
  gst_object_unref(element);
  return linked;
}

// One rule for every demuxer pad: the branch a pad reaches is decided by the
// pad's caps, never by the order pads are announced in. A pad whose caps no
// branch handles is linked to the drop funnel — a tsdemux src pad left unlinked
// fails the whole pipeline with "streaming stopped, reason=not-linked", so every
// pad gets a peer, and no pad is ever handed to a parser that cannot take it.
// The copy path's demux ("d") has a video branch; the transcode path's ("ad")
// does not, because its video comes from the frame ring.
auto output::demux_pad_added(GstElement* demux,
                             GstPad* pad,
                             gpointer self) -> void
{
  auto* out = static_cast<output*>(self);
  // Which branch this pad can feed is decided by the caps it arrives with --
  // never by its pad name and never by the order tsdemux announces its pads in.
  // A TS can expose its audio pad first, and a chained template link would then
  // put audio into the video branch.
  std::string media;
  bool aac = false;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps != nullptr) {
    if (gst_caps_get_size(caps) > 0) {
      const GstStructure* str = gst_caps_get_structure(caps, 0);
      if (const char* name = gst_structure_get_name(str); name != nullptr) {
        media = name;
      }
      gint version = 0;
      aac = gst_structure_has_name(str, "audio/mpeg")
          && gst_structure_get_int(str, "mpegversion", &version)
          && (version == 4 || version == 2);
    }
    gst_caps_unref(caps);
  }
  const bool video = media.starts_with("video/");
  const bool audio = media.starts_with("audio/");
  const bool copy_path = g_strcmp0(GST_OBJECT_NAME(demux), "d") == 0;

  if (audio) {
    if (!aac) {
      // An AC-3 (or other) audio ES cannot enter a branch declared as AAC-LC,
      // so it is sunk and reported instead of negotiating a link it can never
      // satisfy. The output publishes video only.
      out->m_audio_dropped.store(true, std::memory_order_relaxed);
      out->log("non-AAC audio ES: output runs video-only (audio_dropped)\n");
      out->sink_into_drop(pad);
      return;
    }
    // Both tiers carry AAC through the same branch: aq -> aacparse -> acaps,
    // attached to the muxer here, i.e. only once the TS has shown it carries
    // audio. A second audio pad, or a link that does not take, is sunk by
    // attach_audio_branch so it cannot stall the muxer.
    out->attach_audio_branch(pad);
    return;
  }

  if (video && copy_path) {
    if (media == "video/x-h264" && out->link_pad_to(pad, "vq", "sink")) {
      // The copy path carries the video ES itself, so this pad has to reach
      // h264parse whichever position the demuxer exposed it in.
      return;
    }
    // FLV carries H.264 only, so any other video ES cannot reach the muxer.
    // Say which one: a copy output fed AV1 or HEVC otherwise publishes nothing
    // and reports no reason.
    out->log(std::format("video ES {} cannot enter the H.264 branch; output "
                         "runs without video\n",
                         media));
  }

  // Video on the transcode path's audio demux (its video comes from the frame
  // ring), a video ES FLV cannot carry, private-data streams, and a pad that
  // arrived without caps. An unlinked demux pad stops the pipeline with
  // "streaming stopped, reason=not-linked", which no reconnect can resolve.
  if (!out->link_pad_to(pad, "ddrop", "sink_%u")) {
    out->log(std::format("demux pad {} has no branch and no drop sink\n",
                         GST_PAD_NAME(pad) != nullptr ? GST_PAD_NAME(pad)
                                                      : "?"));
  }
}

auto output::destroy_pipeline() -> void
{
  GstElement* pipeline = nullptr;
  {
    std::lock_guard<std::mutex> guard(m_pipe_mutex);
    pipeline = m_pipeline;
    m_pipeline = nullptr;
  }
  if (pipeline != nullptr) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }
  if (m_bus != nullptr) {
    gst_object_unref(m_bus);
    m_bus = nullptr;
  }
  if (m_appsrc != nullptr) {
    gst_object_unref(m_appsrc);
    m_appsrc = nullptr;
  }
  if (m_audio_appsrc != nullptr) {
    gst_object_unref(m_audio_appsrc);
    m_audio_appsrc = nullptr;
  }
  if (pipeline != nullptr) {
    gst_object_unref(pipeline);
  }
}

// ---------------------------------------------------------------------------
// Error classification (§3.6, M1.4)
// ---------------------------------------------------------------------------

auto output::is_codec_terminal(const GError* err, GstElement* src) const
    -> bool
{
  if (m_cfg.type != output_proto::rtmp && m_cfg.type != output_proto::rtmps) {
    return false;  // passthrough outputs have no codec constraint
  }
  // A transcode output was preflighted at /start; a runtime negotiation or
  // encoder failure is retried forever like any other output failure rather
  // than latching a terminal state (the FLV codec rule below governs the
  // copy-only path, where a non-H.264 ES genuinely cannot self-heal).
  if (m_cfg.transcode.target != transcode_target::none) {
    return false;
  }
  // A caps-negotiation / format failure on the FLV path means the elementary
  // stream cannot enter FLV (e.g. H.265 video against flvmux) — that cannot
  // self-heal, so it is the one terminal class. Everything else (connection
  // refused/reset, DNS, auth-shaped closes, timeouts) retries forever.
  const bool format_error =
      (err->domain == GST_STREAM_ERROR
       && (err->code == GST_STREAM_ERROR_FORMAT
           || err->code == GST_STREAM_ERROR_WRONG_TYPE
           || err->code == GST_STREAM_ERROR_CODEC_NOT_FOUND))
      || (err->domain == GST_CORE_ERROR
          && err->code == GST_CORE_ERROR_NEGOTIATION);
  if (!format_error) {
    return false;
  }
  // Only when raised by the mux/parse chain — a sink can also raise
  // WRONG_TYPE-ish errors for transport reasons.
  if (src == nullptr) {
    return true;
  }
  gchar* name = gst_element_get_name(src);
  const bool from_mux_chain = name != nullptr
      && (g_str_has_prefix(name, "mux") || g_str_has_prefix(name, "h264parse")
          || g_str_has_prefix(name, "aacparse")
          || g_str_has_prefix(name, "d"));
  g_free(name);
  return from_mux_chain;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

auto output::start() -> bool
{
  const bool is_transcode =
      m_cfg.transcode.target != transcode_target::none;
  // An RTMP/RTMPS transcode output still needs a ts_ring cursor for its AAC
  // audio branch; a video-only srt/rist transcode output does not, so it
  // takes no ts_ring drops.
  const bool needs_ts = !is_transcode || m_cfg.type == output_proto::rtmp
      || m_cfg.type == output_proto::rtmps;

  if (needs_ts) {
    m_consumer = m_ring.add_consumer();
    if (m_consumer < 0) {
      set_last_error("no ring consumer slot");
      set_state(run_state::error);
      return false;
    }
  }
  if (is_transcode) {
    if (m_frames == nullptr) {
      if (m_consumer >= 0) {
        m_ring.remove_consumer(m_consumer);
        m_consumer = -1;
      }
      set_last_error("transcode output has no frame ring");
      set_state(run_state::error);
      return false;
    }
    m_frame_consumer = m_frames->add_consumer();
    if (m_frame_consumer < 0) {
      if (m_consumer >= 0) {
        m_ring.remove_consumer(m_consumer);
        m_consumer = -1;
      }
      set_last_error("no frame ring consumer slot");
      set_state(run_state::error);
      return false;
    }
    m_frame_buf.resize(m_frames->capacity());
  }
  m_stopping.store(false, std::memory_order_release);
  set_state(run_state::starting);
  m_thread = std::thread([this]() -> void { worker(); });
  return true;
}

auto output::stop() -> void
{
  m_stopping.store(true, std::memory_order_release);
  // A feed thread waiting for the appsrc to drain returns on this rather than
  // on its next poll interval.
  m_feed_cv.notify_all();
  // The pipeline goes to NULL BEFORE the join. A worker blocked inside a
  // GStreamer call (a sink connecting, an appsrc push) never observes m_stopping,
  // and the NULL transition is what releases it -- so the join must not be
  // attempted until the pipeline is down, or stop blocks for as long as the
  // block lasts. destroy_pipeline() issues that transition and is idempotent.
  if (GstElement* pipeline_now = pipeline(); pipeline_now != nullptr) {
    gst_element_set_state(pipeline_now, GST_STATE_NULL);
    gst_object_unref(pipeline_now);
  }
  if (m_thread.joinable()) {
    m_thread.join();
  }
  destroy_pipeline();
  if (m_consumer >= 0) {
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
  }
  if (m_frame_consumer >= 0 && m_frames != nullptr) {
    m_frames->remove_consumer(m_frame_consumer);
    m_frame_consumer = -1;
  }
  if (m_state.load(std::memory_order_acquire) != run_state::error) {
    set_state(run_state::stopped);
  }
}

auto output::worker() -> void
{
  auto backoff = std::chrono::duration_cast<std::chrono::milliseconds>(
      k_backoff_initial);

  bool first_attempt = true;
  while (!m_stopping.load(std::memory_order_acquire)) {
    // A pinned address is a property of the ATTEMPT, not of the session: the
    // /start pin is used as-is for the first connect, and every reconnect
    // re-resolves and re-vets the name first. A platform that retires or
    // rebalances an address therefore costs one backoff interval, not the rest
    // of the session. The egress policy is applied to the fresh answer, so a
    // name that rebinds to a private address is refused and the previous pin is
    // kept (the connect then fails on its own, visibly) rather than followed.
    if (!first_attempt && m_repin
        && !m_stopping.load(std::memory_order_acquire)) {
      output_config fresh = m_cfg;
      if (m_repin(fresh)) {
        if (fresh.pinned_ip != m_cfg.pinned_ip) {
          log("re-resolved " + m_cfg.host + ": new address\n");
          m_cfg.pinned_ip = fresh.pinned_ip;
        }
      } else {
        log("re-resolve refused; keeping the pinned address\n");
      }
    }
    first_attempt = false;

    const auto attempt_started = std::chrono::steady_clock::now();
    const bool retryable = run_once();
    destroy_pipeline();
    if (!retryable) {
      return;  // terminal: state already error
    }
    if (m_stopping.load(std::memory_order_acquire)) {
      return;
    }

    // §3.6 backoff: 1 s → ×2 → cap 30 s, forever. A run that stayed up past
    // k_stable_after resets the ladder.
    if (std::chrono::steady_clock::now() - attempt_started > k_stable_after) {
      backoff = std::chrono::duration_cast<std::chrono::milliseconds>(
          k_backoff_initial);
    }
    set_state(run_state::reconnecting);
    m_reconnects.fetch_add(1, std::memory_order_relaxed);
    log(std::format("reconnecting in {} ms\n", backoff.count()));

    const auto wake = std::chrono::steady_clock::now() + backoff;
    while (!m_stopping.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < wake)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    backoff = std::min(
        backoff * 2,
        std::chrono::duration_cast<std::chrono::milliseconds>(k_backoff_cap));
  }
}

auto output::run_once() -> bool
{
  set_state(run_state::starting);
  m_audio_dropped.store(false, std::memory_order_relaxed);

  std::string err;
  if (!build_pipeline(err)) {
    set_last_error(err);
    log("pipeline build failed: " + err + "\n");
    return true;  // element presence was pre-checked; treat as retryable
  }

  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE)
  {
    set_last_error("pipeline refused PLAYING");
    return true;
  }

  m_run_started = std::chrono::steady_clock::now();
  m_audio_linked.store(false, std::memory_order_relaxed);
  m_first_video_push = std::chrono::steady_clock::time_point {};
  m_audio_probe_done = false;
  m_muxer_audio_logged.store(false);
  m_first_output_logged.store(false);
  m_feed = feed_stats {};
  m_feed.since = m_run_started;

  std::vector<uint8_t> buf(k_read_chunk);
  bool announced_running = false;
  bool video_caps_set = false;

  while (!m_stopping.load(std::memory_order_acquire)) {
    // 1) bus: errors / EOS / state transitions, non-blocking.
    while (m_bus != nullptr) {
      GstMessage* msg = gst_bus_pop_filtered(
          m_bus,
          static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS
                                      | GST_MESSAGE_STATE_CHANGED));
      if (msg == nullptr) {
        break;
      }
      bool failed = false;
      bool terminal = false;
      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
          GError* gerr = nullptr;
          gchar* dbg = nullptr;
          gst_message_parse_error(msg, &gerr, &dbg);
          const std::string text = redact_secret(
              (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                            : "unknown",
              m_cfg.key_or_streamid);
          // The element name is ours (venc/mux/h264parse/...), so naming it is
          // safe and it is the only way to tell a negotiation failure in the
          // encoder chain from the sink's transport error. The GError domain,
          // code and debug string are logged too: "Internal data stream error."
          // is the same text for a negotiation failure, a not-negotiated flow
          // return and a broken pad link, so the message alone cannot be acted
          // on. Both the message and the debug string go through the same
          // redaction as last_error.
          GstObject* src_obj = GST_MESSAGE_SRC(msg);
          const char* who = (src_obj != nullptr && GST_OBJECT_NAME(src_obj))
              ? GST_OBJECT_NAME(src_obj) : "?";
          const char* domain = (gerr != nullptr && gerr->domain != 0)
              ? g_quark_to_string(gerr->domain) : "none";
          const int code = gerr != nullptr ? gerr->code : 0;
          const std::string debug = redact_secret(
              dbg != nullptr ? dbg : "", m_cfg.key_or_streamid);
          terminal = is_codec_terminal(gerr, GST_ELEMENT(src_obj));
          set_last_error(terminal ? "rtmp_codec_unsupported" : text);
          log(std::format("bus error from {}: domain={} code={} ({}) debug={}\n",
                          who, domain != nullptr ? domain : "none", code, text,
                          debug.empty() ? "-" : debug));
          if (gerr != nullptr) {
            g_error_free(gerr);
          }
          g_free(dbg);
          failed = true;
          break;
        }
        case GST_MESSAGE_EOS:
          set_last_error("unexpected EOS");
          failed = true;
          break;
        case GST_MESSAGE_STATE_CHANGED: {
          if (GST_MESSAGE_SRC(msg) == GST_OBJECT(m_pipeline)) {
            GstState newstate = GST_STATE_VOID_PENDING;
            gst_message_parse_state_changed(msg, nullptr, &newstate, nullptr);
            if (newstate == GST_STATE_PLAYING && !announced_running) {
              announced_running = true;
              set_state(run_state::running);
              log("running\n");
            }
          }
          break;
        }
        default:
          break;
      }
      gst_message_unref(msg);
      if (failed) {
        if (terminal) {
          set_state(run_state::error);
          log("terminal: rtmp_codec_unsupported\n");
          return false;
        }
        return true;
      }
    }

    // 2) feed: ring → appsrc. The push never blocks, so a stalled sink leaves
    // this cursor behind and the ring's drop-oldest takes over — ingest never
    // notices.
    if (m_cfg.transcode.target != transcode_target::none) {
      if (!feed_transcode(video_caps_set)) {
        return true;  // attempt over: flushing appsrc or stopping
      }
      continue;
    }
    const std::size_t got =
        m_ring.read(m_consumer, buf.data(), buf.size(), k_read_timeout);
    if (got == 0) {
      continue;  // timeout tick: re-check bus/stop; or ring closed → stop soon
    }
    GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, got, nullptr);
    gst_buffer_fill(gbuf, 0, buf.data(), got);
    if (gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), gbuf) != GST_FLOW_OK) {
      set_last_error("appsrc rejected buffer (pipeline flushing)");
      return true;
    }
    m_bytes_sent.fetch_add(got, std::memory_order_relaxed);
  }
  return true;  // stopping
}

// flvmux emits nothing until its audio pad carries data, so an audio pad it was
// given but never receives data on holds the sink in PAUSED with no error
// anywhere. The muxer's audio pad is therefore requested HERE, from the demux's
// audio pad-added signal: the presence of that pad is the TS saying it carries
// an audio stream. A video-only ingest never gets this far and flvmux runs
// with a video pad alone, so nothing time-based decides whether video publishes.
auto output::attach_audio_branch(GstPad* demux_pad) -> void
{
  if (m_audio_linked.exchange(true)) {
    // One muxer audio pad per output; a second audio ES has nowhere to go.
    sink_into_drop(demux_pad);
    log("second audio pad from the source TS; the output keeps the first\n");
    return;
  }

  GstElement* aq = gst_bin_get_by_name(GST_BIN(m_pipeline), "aq");
  GstElement* acaps = gst_bin_get_by_name(GST_BIN(m_pipeline), "acaps");
  GstElement* mux = gst_bin_get_by_name(GST_BIN(m_pipeline), "mux");
  if (aq == nullptr || acaps == nullptr || mux == nullptr) {
    if (aq != nullptr) { gst_object_unref(aq); }
    if (acaps != nullptr) { gst_object_unref(acaps); }
    if (mux != nullptr) { gst_object_unref(mux); }
    m_audio_dropped.store(true, std::memory_order_relaxed);
    return;
  }

  GstPad* aq_sink = gst_element_get_static_pad(aq, "sink");
  const GstPadLinkReturn lr = (aq_sink != nullptr && !gst_pad_is_linked(aq_sink))
      ? gst_pad_link(demux_pad, aq_sink)
      : GST_PAD_LINK_REFUSED;
  if (aq_sink != nullptr) {
    gst_object_unref(aq_sink);
  }
  if (lr != GST_PAD_LINK_OK) {
    log(std::format("audio pad link into the AAC branch failed ({}); output "
                    "runs video-only\n",
                    gst_pad_link_get_name(lr)));
    m_audio_dropped.store(true, std::memory_order_relaxed);
    sink_into_drop(demux_pad);
    gst_object_unref(aq);
    gst_object_unref(acaps);
    gst_object_unref(mux);
    return;
  }

  GstPad* caps_src = gst_element_get_static_pad(acaps, "src");
  // flvmux/eflvmux expose the audio pad template as the literal name "audio",
  // not "audio_%u"; request that first and only fall back to a numbered
  // template.
  GstPad* mux_audio = gst_element_request_pad_simple(mux, "audio");
  if (mux_audio == nullptr) {
    mux_audio = gst_element_request_pad_simple(mux, "audio_%u");
  }
  bool linked = false;
  if (caps_src != nullptr && mux_audio != nullptr) {
    gst_pad_add_probe(mux_audio, GST_PAD_PROBE_TYPE_BUFFER, &count_buffer_probe,
                      &m_audio_frames, nullptr);
    gst_pad_add_probe(mux_audio, GST_PAD_PROBE_TYPE_BUFFER,
                      &output::muxer_audio_first_buffer, this, nullptr);
    linked = gst_pad_link(caps_src, mux_audio) == GST_PAD_LINK_OK;
    if (!linked) {
      // A requested-but-unlinked audio pad is exactly the state that stalls
      // flvmux, so it is handed back rather than left on the muxer.
      gst_element_release_request_pad(mux, mux_audio);
    }
  }
  if (!linked) {
    log("AAC branch could not be attached to the muxer; output runs "
        "video-only\n");
    m_audio_dropped.store(true, std::memory_order_relaxed);
  } else {
    log("source TS carries audio; muxer audio pad linked\n");
  }
  if (caps_src != nullptr) {
    gst_object_unref(caps_src);
  }
  if (mux_audio != nullptr) {
    gst_object_unref(mux_audio);
  }
  gst_object_unref(aq);
  gst_object_unref(acaps);
  gst_object_unref(mux);
}

// A demux src pad with nowhere to go fails the pipeline with "streaming
// stopped, reason=not-linked", so pads the output does not consume are sunk
// into the drop funnel.
auto output::sink_into_drop(GstPad* pad) -> void
{
  GstElement* ddrop = gst_bin_get_by_name(GST_BIN(m_pipeline), "ddrop");
  if (ddrop == nullptr) {
    return;
  }
  GstPad* sink = gst_element_request_pad_simple(ddrop, "sink_%u");
  if (sink != nullptr) {
    if (!gst_pad_is_linked(sink)) {
      const GstPadLinkReturn lr = gst_pad_link(pad, sink);
      if (lr != GST_PAD_LINK_OK) {
        log(std::format("pad {} could not be sunk ({}); it stays unlinked\n",
                        GST_PAD_NAME(pad) != nullptr ? GST_PAD_NAME(pad) : "?",
                        gst_pad_link_get_name(lr)));
      }
    }
    gst_object_unref(sink);
  }
  gst_object_unref(ddrop);
}

// Transcode feed: encoded VIDEO comes from the shared decode frame ring; AAC
// AUDIO (rtmp/rtmps only) is copied from the ts_ring, exactly as the copy-only
// path does, so RTMP audio passthrough is unchanged. Returns true when the
// caller should stop the attempt (appsrc gone).
auto output::feed_transcode(bool& video_caps_set) -> bool
{
  const guint64 cap = gst_app_src_get_max_bytes(GST_APP_SRC(m_appsrc));
  const guint64 level =
      gst_app_src_get_current_level_bytes(GST_APP_SRC(m_appsrc));
  ++m_feed.iters;
  m_feed.level_peak = std::max(m_feed.level_peak, static_cast<uint64_t>(level));
  if (m_feed.since == std::chrono::steady_clock::time_point {}) {
    m_feed.since = std::chrono::steady_clock::now();
  }

  if (level >= cap)
  {
    // Backpressure, not a fixed pause: wait for a buffer to leave the appsrc
    // and then push the frame that is next in line. Sleeping for a fixed slice
    // per pass capped the feed at one frame per slice and, with a cap only
    // ~3 frames deep, held it well below the ingest rate while the ring threw
    // the difference away.
    ++m_feed.blocked;
    const auto blocked_started = std::chrono::steady_clock::now();
    const bool room = wait_appsrc_drain(cap);
    m_feed.blocked_ms += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - blocked_started)
                             .count();
    if (!room) {
      report_feed();
      return false;
    }
  }

  // Audio first: the TS is what proves an audio stream exists, so the demux's
  // audio pad is seen (and the muxer's audio pad requested) as early as
  // possible. A source with no audio simply never links one.
  if (m_audio_appsrc != nullptr) {
    std::vector<uint8_t> abuf(k_read_chunk);
    for (;;) {
      const std::size_t agot = m_ring.read(
          m_consumer, abuf.data(), abuf.size(), std::chrono::milliseconds(0));
      if (agot == 0) {
        break;
      }
      GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, agot, nullptr);
      gst_buffer_fill(gbuf, 0, abuf.data(), agot);
      if (gst_app_src_push_buffer(GST_APP_SRC(m_audio_appsrc), gbuf)
          != GST_FLOW_OK)
      {
        break;
      }
      m_bytes_sent.fetch_add(agot, std::memory_order_relaxed);
    }
  }

  frame_ring::frame_meta vmeta;
  const auto read_started = std::chrono::steady_clock::now();
  const std::size_t vgot =
      m_frames->read(m_frame_consumer, vmeta, m_frame_buf.data(),
                     m_frame_buf.size(), k_read_timeout);
  m_feed.read_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - read_started)
                        .count();
  if (vgot > 0) {
    ++m_feed.consumed;
    if (!video_caps_set) {
      set_video_caps(vgot, vmeta);
      video_caps_set = true;
    }
    log_muxer_input();
    GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, vgot, nullptr);
    gst_buffer_fill(gbuf, 0, m_frame_buf.data(), vgot);
    GST_BUFFER_PTS(gbuf) = static_cast<GstClockTime>(vmeta.pts);
    GST_BUFFER_DTS(gbuf) = static_cast<GstClockTime>(vmeta.dts);
    if ((vmeta.flags & frame_ring::k_flag_keyframe) == 0U) {
      GST_BUFFER_FLAG_SET(gbuf, GST_BUFFER_FLAG_DELTA_UNIT);
    }
    const auto push_started = std::chrono::steady_clock::now();
    const GstFlowReturn pushed =
        gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), gbuf);
    m_feed.push_ms += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - push_started)
                          .count();
    if (pushed != GST_FLOW_OK) {
      set_last_error("appsrc rejected buffer (pipeline flushing)");
      return false;
    }
    ++m_feed.pushed;
    m_bytes_sent.fetch_add(vgot, std::memory_order_relaxed);
    if (m_first_video_push == std::chrono::steady_clock::time_point {}) {
      m_first_video_push = std::chrono::steady_clock::now();
    }
  }

  report_feed();
  return !m_stopping.load(std::memory_order_acquire);
}

auto output::muxer_first_buffer(GstPad* /*pad*/, GstPadProbeInfo* /*info*/,
                                gpointer self) -> GstPadProbeReturn
{
  auto* out = static_cast<output*>(self);
  if (!out->m_first_output_logged.exchange(true)
      && out->m_first_video_push != std::chrono::steady_clock::time_point {}) {
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now()
                          - out->m_first_video_push)
                          .count();
    out->log(std::format("first published frame: {:.0f} ms after the first "
                         "decoded frame reached the encoder\n",
                         ms));
  }
  return GST_PAD_PROBE_PASS;
}

auto output::muxer_audio_first_buffer(GstPad* pad, GstPadProbeInfo* /*info*/,
                                      gpointer self) -> GstPadProbeReturn
{
  auto* out = static_cast<output*>(self);
  if (out->m_muxer_audio_logged.exchange(true)) {
    // Report once: the caps of one audio stream do not change mid-attempt in
    // the way a video sequence header can.
    return GST_PAD_PROBE_REMOVE;
  }
  GstCaps* caps = gst_pad_get_current_caps(pad);
  std::string rendered = "none";
  if (caps != nullptr) {
    gchar* text = gst_caps_to_string(caps);
    if (text != nullptr) {
      rendered = text;
      g_free(text);
    }
    gst_caps_unref(caps);
  }
  out->log(std::format("output {}: muxer audio in: {}\n", out->m_cfg.id,
                       rendered));
  return GST_PAD_PROBE_REMOVE;
}

auto output::appsrc_drained(GstPad* /*pad*/, GstPadProbeInfo* /*info*/,
                            gpointer self) -> GstPadProbeReturn
{
  // Runs on the appsrc's streaming thread, which also holds the appsrc's own
  // object lock; taking m_feed_mutex here would invert against the feed
  // thread's level query. The wait is timed instead, so a wakeup that races
  // the feed thread's level check costs one poll interval, not a frame.
  static_cast<output*>(self)->m_feed_cv.notify_all();
  return GST_PAD_PROBE_PASS;
}

auto output::wait_appsrc_drain(guint64 cap) -> bool
{
  std::unique_lock<std::mutex> lock(m_feed_mutex);
  const auto started = std::chrono::steady_clock::now();
  while (!m_stopping.load(std::memory_order_acquire)
         && gst_app_src_get_current_level_bytes(GST_APP_SRC(m_appsrc)) >= cap)
  {
    if (std::chrono::steady_clock::now() - started > k_stall_timeout) {
      lock.unlock();
      set_last_error("output sink stalled");
      log("sink stalled with a full appsrc; rebuilding the output\n");
      return false;
    }
    m_feed_cv.wait_for(lock, std::chrono::milliseconds(50));
  }
  return !m_stopping.load(std::memory_order_acquire);
}

auto output::report_feed() -> void
{
  const auto now = std::chrono::steady_clock::now();
  if (m_feed.since == std::chrono::steady_clock::time_point {}) {
    m_feed.since = now;
    return;
  }
  const double elapsed = std::chrono::duration<double>(now - m_feed.since).count();
  if (now - m_feed.since < k_feed_stats_period) {
    return;
  }
  const double read_ms = m_feed.consumed > 0
      ? m_feed.read_ms / static_cast<double>(m_feed.consumed) : 0.0;
  const double push_ms = m_feed.pushed > 0
      ? m_feed.push_ms / static_cast<double>(m_feed.pushed) : 0.0;
  const double blocked_ms = m_feed.iters > 0
      ? m_feed.blocked_ms / static_cast<double>(m_feed.iters) : 0.0;
  log(std::format(
      "transcode feed: iters={} consumed={} pushed={} blocked={} ({} ms/iter, "
      "{} ms total) level={}/{} KiB peak={} KiB read={:.2f} ms push={:.2f} ms "
      "enc_in={} mux_out={} ring_drops={} in {:.1f} s\n",
      m_feed.iters, m_feed.consumed, m_feed.pushed, m_feed.blocked,
      blocked_ms, m_feed.blocked_ms,
      gst_app_src_get_current_level_bytes(GST_APP_SRC(m_appsrc)) / 1024,
      gst_app_src_get_max_bytes(GST_APP_SRC(m_appsrc)) / 1024,
      m_feed.level_peak / 1024, read_ms, push_ms,
      m_encoder_in.load(std::memory_order_relaxed) - m_feed.encoder_in,
      m_muxer_out.load(std::memory_order_relaxed) - m_feed.muxer_out,
      m_frames->dropped_frames(m_frame_consumer), elapsed));
  const uint64_t enc_in = m_encoder_in.load(std::memory_order_relaxed);
  const uint64_t mux_out = m_muxer_out.load(std::memory_order_relaxed);
  m_feed = feed_stats {};
  m_feed.encoder_in = enc_in;
  m_feed.muxer_out = mux_out;
  m_feed.since = now;
}

auto output::log_muxer_input() -> void
{
  if (m_muxer_input_logged || m_pipeline == nullptr) {
    return;
  }
  GstElement* mux = gst_bin_get_by_name(GST_BIN(m_pipeline), "mux");
  if (mux == nullptr) {
    return;
  }
  std::string rendered;
  GstIterator* it = gst_element_iterate_sink_pads(mux);
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done && rendered.empty()) {
    switch (gst_iterator_next(it, &item)) {
      case GST_ITERATOR_OK: {
        auto* pad = GST_PAD(g_value_get_object(&item));
        const char* name = GST_PAD_NAME(pad);
        const bool audio = name != nullptr && g_str_has_prefix(name, "audio");
        if (!audio) {
          GstCaps* caps = gst_pad_get_current_caps(pad);
          if (caps != nullptr) {
            gchar* text = gst_caps_to_string(caps);
            if (text != nullptr) {
              rendered = text;
              g_free(text);
            }
            gst_caps_unref(caps);
          }
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
  if (rendered.empty()) {
    return;
  }
  // Profile, chroma format and bit depth are what a destination refuses first,
  // so they are reported from the pad the muxer actually receives.
  m_muxer_input_logged = true;
  log(std::format("output {}: muxer video in: {}\n", m_cfg.id, rendered));
}

auto output::set_video_caps(std::size_t frame_bytes,
                            const frame_ring::frame_meta& meta) -> void
{
  // Raw NV12 from the decode stage. The frame ring carries format + stride,
  // not dimensions: width follows the line stride and height is derived from
  // the NV12 plane layout (bytes = w*h*3/2). The encoder needs fixed
  // width/height, so they are reconstructed here.
  const int width = meta.stride > 0 ? meta.stride : 0;
  const int height =
      (meta.stride > 0)
      ? static_cast<int>((static_cast<std::uint64_t>(frame_bytes) * 2U)
                         / (3U * static_cast<std::uint64_t>(meta.stride)))
      : 0;
  GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                      "format", G_TYPE_STRING, "NV12",
                                      "width", G_TYPE_INT, width,
                                      "height", G_TYPE_INT, height,
                                      nullptr);
  gst_app_src_set_caps(GST_APP_SRC(m_appsrc), caps);
  gst_caps_unref(caps);

  // The cap is now known in frames, so it can be expressed in frames: the
  // buffer holds k_appsrc_frame_span of them whatever the resolution.
  const guint64 cap = std::clamp<guint64>(
      static_cast<guint64>(frame_bytes) * k_appsrc_frame_span,
      k_appsrc_default_bytes, k_appsrc_ceiling_bytes);
  gst_app_src_set_max_bytes(GST_APP_SRC(m_appsrc), cap);
  log(std::format("video appsrc capped at {} KiB ({} frames of {} B)\n",
                  cap / 1024, k_appsrc_frame_span, frame_bytes));
}
