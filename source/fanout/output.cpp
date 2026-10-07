// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
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
constexpr guint64 k_appsrc_max_bytes = 4 * 1024 * 1024;

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
}  // namespace

output::output(output_config cfg, ts_ring& ring, frame_ring* frames, log_fn log)
    : m_cfg {std::move(cfg)}
    , m_ring {ring}
    , m_frames {frames}
    , m_log {std::move(log)}
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
  constexpr std::string_view k_appsrc =
      "appsrc name=osrc is-live=true do-timestamp=true format=time "
      "block=true max-bytes=4194304 ";
  constexpr std::string_view k_audio_appsrc =
      "appsrc name=asrc is-live=true do-timestamp=true format=time "
      "block=true max-bytes=4194304 ";

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
        return std::string {k_appsrc}
            + "! tsparse set-timestamps=true alignment=7 "
              "! tsdemux name=d "
              "d. ! queue ! h264parse config-interval=-1 "
              "! video/x-h264,stream-format=avc,alignment=au ! mux. "
              "d. ! queue ! aacparse "
              "! audio/mpeg,mpegversion=4,stream-format=raw ! mux. "
              "flvmux name=mux streamable=true latency=1000000000 "
              "! rtmp2sink name=osink async-connect=true";
    }
    return {};
  }

  // Transcode targets are h264, h265 and av1. av1 is representable, but only over
  // the MPEG-TS carriers: flvmux/eflvmux expose no video/x-av1 caps, so
  // validate_transcode_targets refuses av1 -> rtmp/rtmps at /start and it can
  // never reach the rtmp cases below.
  const std::string enc = encoder != nullptr ? encoder : "";
  const std::string parse = transcode_target_parser(transcode.target);

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
      return std::format(
          "{}! queue ! {}{} name=venc ! {} config-interval=-1 ! {} ! mux. "
          "{}! tsparse set-timestamps=true alignment=7 ! tsdemux name=d "
          "d. ! queue ! aacparse "
          "! audio/mpeg,mpegversion=4,stream-format=raw ! mux. "
          "{} name=mux streamable=true "
          "! rtmp2sink name=osink async-connect=true",
          k_appsrc,
          scale,
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
          "{}! queue ! {}{} name=venc ! {} config-interval=-1 "
          "! mpegtsmux alignment=7{} "
          "! srtsink name=osink wait-for-connection=false",
          k_appsrc,
          scale,
          enc,
          parse,
          custom);
    }
    case output_proto::rist: {
      const char* custom = transcode.target == transcode_target::av1
          ? " enable-custom-mappings=true" : "";
      return std::format(
          "{}! queue ! {}{} name=venc ! {} config-interval=-1 "
          "! mpegtsmux alignment=7{} ! ristsink name=osink",
          k_appsrc,
          scale,
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
  m_pipeline = gst_parse_launch(tmpl.c_str(), &gerr);
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

  gst_app_src_set_max_bytes(GST_APP_SRC(m_appsrc), k_appsrc_max_bytes);

  if (is_transcode) {
    m_audio_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "asrc");
    if (m_audio_appsrc != nullptr) {
      gst_app_src_set_max_bytes(GST_APP_SRC(m_audio_appsrc),
                                k_appsrc_max_bytes);
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
      if (m_cfg.transcode.bitrate_kbps != 0) {
        if (has_property(venc, "bitrate")) {
          g_object_set(venc,
                       "bitrate",
                       static_cast<gint>(m_cfg.transcode.bitrate_kbps),
                       nullptr);
        }
      } else {
        log(std::format(
            "output {}: transcode has no bitrate_kbps; the hardware encoder will "
            "auto-calculate a near-lossless rate\n",
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
      // exactly how a silent miss here hides.
      {
        gint applied_bitrate = -1;
        guint applied_gop = 0;
        if (has_property(venc, "bitrate")) {
          g_object_get(venc, "bitrate", &applied_bitrate, nullptr);
        }
        if (has_property(venc, "key-int-max")) {
          g_object_get(venc, "key-int-max", &applied_gop, nullptr);
        }
        log(std::format(
            "output {}: encoder {} bitrate={} kbps gop={} (requested {}/{})\n",
            m_cfg.id, chosen_encoder, applied_bitrate, applied_gop,
            m_cfg.transcode.bitrate_kbps, m_cfg.transcode.gop));
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
      // audio_dropped=true instead of failing the output.
      GstElement* demux = gst_bin_get_by_name(GST_BIN(m_pipeline), "d");
      if (demux != nullptr) {
        g_signal_connect(demux, "pad-added",
                         G_CALLBACK(&output::demux_pad_added), this);
        gst_object_unref(demux);
      }
      break;
    }
  }
  gst_object_unref(sink);

  m_bus = gst_element_get_bus(m_pipeline);
  return true;
}

auto output::demux_pad_added(GstElement* /*demux*/,
                             GstPad* pad,
                             gpointer self) -> void
{
  auto* out = static_cast<output*>(self);
  gchar* name = gst_pad_get_name(pad);
  const bool is_audio = name != nullptr && g_str_has_prefix(name, "audio");
  g_free(name);
  if (!is_audio) {
    return;
  }
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps == nullptr) {
    return;
  }
  bool aac = false;
  if (gst_caps_get_size(caps) > 0) {
    const GstStructure* str = gst_caps_get_structure(caps, 0);
    gint version = 0;
    aac = gst_structure_has_name(str, "audio/mpeg")
        && gst_structure_get_int(str, "mpegversion", &version)
        && (version == 4 || version == 2);
  }
  gst_caps_unref(caps);
  if (!aac) {
    out->m_audio_dropped.store(true, std::memory_order_relaxed);
    out->log("non-AAC audio ES: output runs video-only (audio_dropped)\n");
  }
}

auto output::destroy_pipeline() -> void
{
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
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
  if (m_pipeline != nullptr) {
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
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
  // The pipeline goes to NULL BEFORE the join. A worker blocked inside a
  // GStreamer call (a sink connecting, an appsrc push) never observes m_stopping,
  // and the NULL transition is what releases it -- so the join must not be
  // attempted until the pipeline is down, or stop blocks for as long as the
  // block lasts. destroy_pipeline() issues that transition and is idempotent.
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
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

  while (!m_stopping.load(std::memory_order_acquire)) {
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
          // The debug string (dbg) can carry URIs/element internals — it is
          // parsed but deliberately never logged; only gerr->message is, and
          // it is redacted below as a belt-and-suspenders guard (F4).
          const std::string text = redact_secret(
              (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                            : "unknown",
              m_cfg.key_or_streamid);
          terminal = is_codec_terminal(
              gerr, GST_ELEMENT(GST_MESSAGE_SRC(msg)));
          set_last_error(terminal ? "rtmp_codec_unsupported" : text);
          log("bus error: " + text + "\n");
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

    // 2) feed: ring → appsrc. block=true bounds only THIS output's feeder;
    // when the sink stalls, the cursor lags and the ring's drop-oldest takes
    // over — ingest never notices.
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

// Transcode feed: encoded VIDEO comes from the shared decode frame ring; AAC
// AUDIO (rtmp/rtmps only) is copied from the ts_ring, exactly as the copy-only
// path does, so RTMP audio passthrough is unchanged. Returns true when the
// caller should stop the attempt (appsrc gone).
auto output::feed_transcode(bool& video_caps_set) -> bool
{
  frame_ring::frame_meta vmeta;
  const std::size_t vgot =
      m_frames->read(m_frame_consumer, vmeta, m_frame_buf.data(),
                     m_frame_buf.size(), k_read_timeout);
  if (vgot > 0) {
    if (!video_caps_set) {
      set_video_caps(vgot, vmeta);
      video_caps_set = true;
    }
    GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, vgot, nullptr);
    gst_buffer_fill(gbuf, 0, m_frame_buf.data(), vgot);
    GST_BUFFER_PTS(gbuf) = static_cast<GstClockTime>(vmeta.pts);
    GST_BUFFER_DTS(gbuf) = static_cast<GstClockTime>(vmeta.dts);
    if ((vmeta.flags & frame_ring::k_flag_keyframe) == 0U) {
      GST_BUFFER_FLAG_SET(gbuf, GST_BUFFER_FLAG_DELTA_UNIT);
    }
    if (gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), gbuf) != GST_FLOW_OK) {
      set_last_error("appsrc rejected buffer (pipeline flushing)");
      return false;
    }
    m_bytes_sent.fetch_add(vgot, std::memory_order_relaxed);
  }

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
  return !m_stopping.load(std::memory_order_acquire);
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
}
