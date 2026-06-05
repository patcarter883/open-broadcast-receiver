#include "restream/restream.h"

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace
{
constexpr int k_mpeg_ts_packet_size = 188;

// How long to wait for tsdemux to expose the elementary-stream pads before
// giving up on detection and falling back to the declared codec / AAC.
constexpr auto k_detect_timeout = std::chrono::seconds {5};

// Shared front of both the detection and the real pipeline: appsrc fed by the
// RIST receiver -> tsparse -> tsdemux. Kept in one place so the two pipelines
// cannot drift (same appsrc tuning, same demux name "demux").
constexpr const char* k_ts_source =
    "appsrc name=videosrc is-live=true do-timestamp=true format=time "
    "stream-type=0 max-bytes=4194304 block=true emit-signals=false "
    "! queue2 ! tsparse set-timestamps=true alignment=7 ! tsdemux name=demux ";

// ---- element-name helpers --------------------------------------------------

auto video_parse(codec cod) noexcept -> const char*
{
  switch (cod) {
    case codec::h264:
      return "h264parse";
    case codec::h265:
      return "h265parse";
    case codec::av1:
      return "av1parse";
  }
  return "h264parse";
}

auto video_caps(codec cod) noexcept -> const char*
{
  switch (cod) {
    case codec::h264:
      return "video/x-h264";
    case codec::h265:
      return "video/x-h265";
    case codec::av1:
      return "video/x-av1";
  }
  return "video/x-h264";
}

// AMD HW path: VA-API (Linux/mesa) when present, else AMF (Windows builds).
// Per-codec elements are still validated in first_missing_element(); this only
// chooses the family. vah264enc presence is the sentinel for the VA path.
auto amd_uses_va() noexcept -> bool
{
  static const bool va = [] {
    GstElementFactory* fac = gst_element_factory_find("vah264enc");
    if (fac != nullptr) {
      gst_object_unref(fac);
      return true;
    }
    return false;
  }();
  return va;
}

// Decoder element for (encoder family, source codec).
auto video_decoder(encoder enc, codec cod) noexcept -> const char*
{
  switch (enc) {
    case encoder::nvenc:
      return cod == codec::h264 ? "nvh264dec"
          : cod == codec::h265  ? "nvh265dec"
                                : "nvav1dec";
    case encoder::qsv:
      return cod == codec::h264 ? "qsvh264dec"
          : cod == codec::h265  ? "qsvh265dec"
                                : "qsvav1dec";
    case encoder::amd:
      if (amd_uses_va()) {
        return cod == codec::h264 ? "vah264dec"
            : cod == codec::h265  ? "vah265dec"
                                  : "vaav1dec";
      }
      [[fallthrough]];
    case encoder::software:
    default:
      return cod == codec::h264 ? "avdec_h264"
          : cod == codec::h265  ? "avdec_h265"
                                : "av1dec";
  }
}

// Just the encoder element factory name (for registry availability checks).
auto video_encoder_element(encoder enc, codec cod) noexcept -> const char*
{
  switch (enc) {
    case encoder::amd:
      if (amd_uses_va()) {
        return cod == codec::h264 ? "vah264enc"
            : cod == codec::h265  ? "vah265enc"
                                  : "vaav1enc";
      }
      return cod == codec::h264 ? "amfh264enc"
          : cod == codec::h265  ? "amfh265enc"
                                : "amfav1enc";
    case encoder::qsv:
      return cod == codec::h264 ? "qsvh264enc"
          : cod == codec::h265  ? "qsvh265enc"
                                : "qsvav1enc";
    case encoder::nvenc:
      return cod == codec::h264 ? "nvh264enc"
          : cod == codec::h265  ? "nvh265enc"
                                : "nvav1enc";
    case encoder::software:
    default:
      return cod == codec::h264 ? "x264enc"
          : cod == codec::h265  ? "x265enc"
                                : "rav1enc";
  }
}

// Full encoder fragment: <enc element> name=videncoder{idx} <params> ! <caps> !
// <out parse> config-interval=1. Mirrors the encoder's kEncoderTemplates.
auto encoder_fragment(encoder enc,
                      codec cod,
                      std::size_t idx,
                      int bitrate) -> std::string
{
  switch (enc) {
    case encoder::amd:
      if (amd_uses_va()) {
        switch (cod) {
          case codec::h264:
            return std::format(
                "vah264enc name=videncoder{0} bitrate={1} rate-control=cbr "
                "target-usage=4 key-int-max=120 b-frames=0 ! "
                "video/x-h264,profile=high ! h264parse config-interval=1",
                idx, bitrate);
          case codec::h265:
            return std::format(
                "vah265enc name=videncoder{0} bitrate={1} rate-control=cbr "
                "target-usage=4 key-int-max=120 b-frames=0 ! video/x-h265 ! "
                "h265parse config-interval=1",
                idx, bitrate);
          case codec::av1:
            return std::format(
                "vaav1enc name=videncoder{0} bitrate={1} rate-control=cbr "
                "target-usage=4 key-int-max=120 ! video/x-av1 ! av1parse",
                idx, bitrate);
        }
        break;
      }
      switch (cod) {
        case codec::h264:
          return std::format(
              "amfh264enc name=videncoder{0} bitrate={1} rate-control=cbr "
              "usage=low-latency preset=quality pre-encode=true "
              "pa-hqmb-mode=auto ! video/x-h264,profile=high ! h264parse "
              "config-interval=1",
              idx, bitrate);
        case codec::h265:
          return std::format(
              "amfh265enc name=videncoder{0} bitrate={1} rate-control=cbr "
              "usage=low-latency preset=quality pre-encode=true "
              "pa-hqmb-mode=auto ! video/x-h265 ! h265parse config-interval=1",
              idx, bitrate);
        case codec::av1:
          return std::format(
              "amfav1enc name=videncoder{0} bitrate={1} rate-control=cbr "
              "usage=low-latency preset=high-quality pre-encode=true "
              "pa-hqmb-mode=auto ! video/x-av1 ! av1parse",
              idx, bitrate);
      }
      break;
    case encoder::qsv:
      switch (cod) {
        case codec::h264:
          return std::format(
              "qsvh264enc name=videncoder{0} bitrate={1} rate-control=cbr "
              "target-usage=1 gop-size=120 ! video/x-h264,profile=high ! "
              "h264parse config-interval=1",
              idx, bitrate);
        case codec::h265:
          return std::format(
              "qsvh265enc name=videncoder{0} bitrate={1} rate-control=cbr "
              "target-usage=1 gop-size=120 ! video/x-h265 ! h265parse "
              "config-interval=1",
              idx, bitrate);
        case codec::av1:
          return std::format(
              "qsvav1enc name=videncoder{0} bitrate={1} rate-control=cbr "
              "target-usage=1 gop-size=120 ! video/x-av1 ! av1parse",
              idx, bitrate);
      }
      break;
    case encoder::nvenc:
      switch (cod) {
        case codec::h264:
          return std::format(
              "nvh264enc name=videncoder{0} bitrate={1} rc-mode=cbr-hq "
              "preset=low-latency-hq gop-size=120 ! h264parse config-interval=1",
              idx, bitrate);
        case codec::h265:
          return std::format(
              "nvh265enc name=videncoder{0} bitrate={1} rc-mode=cbr-hq "
              "preset=low-latency-hq gop-size=120 ! h265parse config-interval=1",
              idx, bitrate);
        case codec::av1:
          return std::format(
              "nvav1enc name=videncoder{0} bitrate={1} rc-mode=cbr "
              "preset=low-latency-hq gop-size=120 ! av1parse",
              idx, bitrate);
      }
      break;
    case encoder::software:
    default:
      switch (cod) {
        case codec::h264:
          return std::format(
              "x264enc name=videncoder{0} bitrate={1} speed-preset=fast "
              "tune=zerolatency key-int-max=120 ! video/x-h264,profile=high ! "
              "h264parse config-interval=1",
              idx, bitrate);
        case codec::h265:
          return std::format(
              "x265enc name=videncoder{0} bitrate={1} speed-preset=fast "
              "tune=zerolatency key-int-max=120 ! video/x-h265 ! h265parse "
              "config-interval=1",
              idx, bitrate);
        case codec::av1:
          return std::format(
              "rav1enc name=videncoder{0} bitrate={1} speed-preset=8 "
              "tile-cols=2 tile-rows=2 ! video/x-av1 ! av1parse",
              idx, bitrate);
      }
      break;
  }
  return {};
}

// Connector between decoder and encoder (handles memory domain + upscale).
auto decode_to_encode_connector(encoder enc,
                                 bool upscale,
                                 int width,
                                 int height) -> std::string
{
  switch (enc) {
    case encoder::nvenc:
      if (upscale) {
        return std::format(
            " ! cudaconvertscale ! "
            "video/x-raw(memory:CUDAMemory),width={},height={} ! ",
            width, height);
      }
      return " ! ";
    case encoder::qsv:
      if (upscale) {
        return std::format(
            " ! vapostproc ! "
            "video/x-raw(memory:VAMemory),width={},height={} ! ",
            width, height);
      }
      return " ! ";
    case encoder::amd:
      if (amd_uses_va()) {
        // Same-vendor VA dec->enc: frames stay in VA memory, link directly;
        // only vapostproc to rescale. No plain videoconvert between va*dec/enc.
        if (upscale) {
          return std::format(
              " ! vapostproc ! "
              "video/x-raw(memory:VAMemory),width={},height={} ! ",
              width, height);
        }
        return " ! ";
      }
      [[fallthrough]];
    case encoder::software:
    default:
      if (upscale) {
        return std::format(
            " ! videoconvert ! videoscale ! video/x-raw,width={},height={} ! ",
            width, height);
      }
      return " ! videoconvert ! ";
  }
}

// The video processing fragment between the source tee and the muxer.
auto video_proc(const destination& dst,
                codec in_codec,
                std::size_t idx) -> std::string
{
  if (!dst.video.reencode) {
    // Copy/passthrough: re-parse to set the muxer stream-format + reinsert
    // SPS/PPS/VPS for late joiners. Parser matches the source codec.
    return std::format("{} config-interval=1", video_parse(in_codec));
  }
  const std::string dec = video_decoder(dst.video.enc, in_codec);
  const std::string conn = decode_to_encode_connector(
      dst.video.enc, dst.video.upscale, dst.video.width, dst.video.height);
  const std::string enc_frag =
      encoder_fragment(dst.video.enc, dst.video.out_codec, idx, dst.video.bitrate_kbps);
  return std::format("{} ! {}{}{}", video_parse(in_codec), dec, conn, enc_frag);
}

// ---- audio element-name helpers --------------------------------------------

// Caps used to pick the demuxed audio pad. AAC and MPEG-1/2 audio (mp2) both
// surface as audio/mpeg; the detected codec — not these caps — selects the
// parser, so a shared filter is fine (there is a single audio elementary
// stream).
auto audio_caps(audio_codec cod) noexcept -> const char*
{
  switch (cod) {
    case audio_codec::aac:
    case audio_codec::mp2:
      return "audio/mpeg";
    case audio_codec::opus:
      return "audio/x-opus";
    case audio_codec::ac3:
      return "audio/x-ac3";
    case audio_codec::eac3:
      return "audio/x-eac3";
  }
  return "audio/mpeg";
}

auto audio_parse(audio_codec cod) noexcept -> const char*
{
  switch (cod) {
    case audio_codec::aac:
      return "aacparse";
    case audio_codec::opus:
      return "opusparse";
    case audio_codec::ac3:
    case audio_codec::eac3:
      return "ac3parse";  // ac3parse handles both AC-3 and E-AC-3
    case audio_codec::mp2:
      return "mpegaudioparse";
  }
  return "aacparse";
}

auto audio_decoder(audio_codec cod) noexcept -> const char*
{
  switch (cod) {
    case audio_codec::aac:
      return "avdec_aac";
    case audio_codec::opus:
      return "avdec_opus";
    case audio_codec::ac3:
      return "avdec_ac3";
    case audio_codec::eac3:
      return "avdec_eac3";
    case audio_codec::mp2:
      return "avdec_mp2float";
  }
  return "avdec_aac";
}

// The output audio codec is always AAC (RTMP/SRT/RIST muxers expect it), so the
// audio branch only passes through when the input is already AAC *and* copy was
// requested. A non-AAC input under copy mode is transparently transcoded to AAC
// (the only correct option for e.g. RTMP) — finish_detection_and_launch() logs
// this upgrade.
auto audio_proc(const destination& dst, audio_codec in_audio, std::size_t idx)
    -> std::string
{
  const bool passthrough = !dst.audio.reencode && in_audio == audio_codec::aac;
  if (passthrough) {
    return "aacparse";
  }
  return std::format(
      "{} ! {} ! audioconvert ! audioresample ! "
      "avenc_aac name=audencoder{} bitrate={}",
      audio_parse(in_audio),
      audio_decoder(in_audio),
      idx,
      dst.audio.bitrate_kbps * 1000);
}

// Muxer + sink fragment for one destination. mux_name is filled with the
// muxer element name so the video/audio branches can link into it. When
// `redact` is true, secrets (stream key / streamid / cname) are replaced with a
// placeholder for safe logging. Every interpolated value is single-quoted so an
// (already validated) value cannot break out of the gst_parse_launch property.
auto output_sink(const destination& dst,
                  std::size_t idx,
                  std::string& mux_name,
                  bool redact) -> std::string
{
  const std::string key = redact && !dst.key_or_streamid.empty()
      ? std::string {"***"}
      : dst.key_or_streamid;
  const std::string cname =
      redact && !dst.cname.empty() ? std::string {"***"} : dst.cname;

  switch (dst.proto) {
    case output_proto::rtmp:
    case output_proto::rtmps: {
      mux_name = std::format("flvmux{}", idx);
      const std::string loc =
          key.empty() ? dst.url : std::format("{}/{}", dst.url, key);
      return std::format(
          "flvmux name={0} streamable=true ! queue ! "
          "rtmp2sink name=osink{1} location='{2}'",
          mux_name, idx, loc);
    }
    case output_proto::srt: {
      mux_name = std::format("mpegtsmux{}", idx);
      // enable-custom-mappings=true: AV1 (and VP9) have no standardised
      // MPEG-TS stream type — mpegtsmux refuses them otherwise ("AV1 requires
      // enabling custom mapping"). The flag is a no-op for H.264/H.265/AAC.
      std::string sink = std::format(
          "mpegtsmux name={0} alignment=7 enable-custom-mappings=true ! queue ! "
          "srtsink name=osink{1} uri='{2}' mode=caller "
          "wait-for-connection=false latency={3}",
          mux_name, idx, dst.url, dst.latency_ms);
      if (!key.empty()) {
        sink += std::format(" streamid='{}'", key);
      }
      return sink;
    }
    case output_proto::rist:
    default: {
      mux_name = std::format("mpegtsmux{}", idx);
      std::string host;
      int port = 0;
      // validated upstream (validate_config); fall back defensively.
      if (!parse_authority(dst.url, host, port)) {
        host = "127.0.0.1";
        port = 5000;
      }
      // ristsink does NOT accept single-quoted property values in
      // gst_parse_launch (unlike rtmp2sink/srtsink) — `address='1.2.3.4'` is a
      // parse error. So address/cname are interpolated UNQUOTED; injection is
      // instead prevented by validate_config, which requires the host (and a
      // non-empty cname) to be a clean token with no whitespace/meta chars.
      // enable-custom-mappings=true so AV1/VP9 can be muxed (see srt branch);
      // a no-op for H.264/H.265/AAC.
      std::string sink = std::format(
          "mpegtsmux name={0} alignment=7 enable-custom-mappings=true ! "
          "rtpmp2tpay ! ristsink name=osink{1} address={2} port={3}",
          mux_name, idx, host, port);
      if (dst.sender_buffer > 0) {
        sink += std::format(" sender-buffer={}", dst.sender_buffer);
      }
      if (!cname.empty()) {
        sink += std::format(" cname={}", cname);
      }
      return sink;
    }
  }
}
}  // namespace

// ---------------------------------------------------------------------------
// restream member implementations
// ---------------------------------------------------------------------------

restream::restream(std::function<void(const std::string&)> log)
    : m_log_func {std::move(log)}
{
}

restream::~restream()
{
  stop();
}

auto restream::log(const std::string& msg) const -> void
{
  if (m_log_func) {
    m_log_func(msg);
  }
}

auto restream::first_missing_element(const receiver_config& cfg,
                                     codec in_video,
                                     audio_codec in_audio) const -> std::string
{
  // A non-AAC input is always decoded + re-encoded to AAC, even under copy mode
  // (see audio_proc) — so the audio decoder is needed whenever reencode is set
  // OR the input is not AAC.
  const bool audio_transcode = in_audio != audio_codec::aac;
  std::vector<std::string> needed;
  for (const destination& dst : cfg.destinations) {
    if (dst.video.reencode) {
      needed.emplace_back(video_decoder(dst.video.enc, in_video));
      needed.emplace_back(video_encoder_element(dst.video.enc, dst.video.out_codec));
      if (dst.video.upscale) {
        if (dst.video.enc == encoder::nvenc) {
          needed.emplace_back("cudaconvertscale");
        } else if (dst.video.enc == encoder::qsv ||
                   (dst.video.enc == encoder::amd && amd_uses_va())) {
          needed.emplace_back("vapostproc");
        }
      }
    }
    if (dst.audio.reencode || audio_transcode) {
      needed.emplace_back(audio_decoder(in_audio));
      needed.emplace_back("avenc_aac");
    }
  }
  for (const std::string& name : needed) {
    GstElementFactory* fac = gst_element_factory_find(name.c_str());
    if (fac == nullptr) {
      return name;
    }
    gst_object_unref(fac);
  }
  return {};
}

auto restream::first_missing_output_element(const receiver_config& cfg) const
    -> std::string
{
  // Only the elements that do NOT depend on the (not-yet-detected) input codec:
  // the output video encoder, the AAC output encoder, and any upscale converter.
  // The input decoders are validated in phase 2 once detection has run.
  std::vector<std::string> needed;
  for (const destination& dst : cfg.destinations) {
    if (dst.video.reencode) {
      needed.emplace_back(
          video_encoder_element(dst.video.enc, dst.video.out_codec));
      if (dst.video.upscale) {
        if (dst.video.enc == encoder::nvenc) {
          needed.emplace_back("cudaconvertscale");
        } else if (dst.video.enc == encoder::qsv ||
                   (dst.video.enc == encoder::amd && amd_uses_va())) {
          needed.emplace_back("vapostproc");
        }
      }
    }
    if (dst.audio.reencode) {
      needed.emplace_back("avenc_aac");
    }
  }
  for (const std::string& name : needed) {
    GstElementFactory* fac = gst_element_factory_find(name.c_str());
    if (fac == nullptr) {
      return name;
    }
    gst_object_unref(fac);
  }
  return {};
}

auto restream::build_pipeline_string(const receiver_config& cfg,
                                     codec in_video,
                                     audio_codec in_audio,
                                     bool redact) -> std::string
{
  // Source + per-stream source fan-out tees. Caps filters after demux select
  // the correct (dynamic) tsdemux pad without locking stream-format, so each
  // per-output parser can negotiate the muxer it feeds independently. The caps
  // come from runtime detection (start_detection), not the declared config.
  std::string pipeline_str = std::format(
      "{}demux. ! {} ! queue ! tee name=vsrc "
      "demux. ! {} ! queue ! tee name=asrc ",
      k_ts_source,
      video_caps(in_video),
      audio_caps(in_audio));

  for (std::size_t idx = 0; idx < cfg.destinations.size(); ++idx) {
    const destination& dst = cfg.destinations[idx];
    std::string mux_name;
    const std::string sink = output_sink(dst, idx, mux_name, redact);

    // video branch: source tee -> processing -> muxer. leaky=downstream so a
    // single stalled endpoint drops buffers on its own branch instead of
    // back-pressuring the shared tee/appsrc (and thereby the RIST ingest).
    pipeline_str += std::format("vsrc. ! queue leaky=downstream ! {} ! {}. ",
                                video_proc(dst, in_video, idx),
                                mux_name);
    // audio branch: source tee -> processing -> muxer
    pipeline_str += std::format(
        "asrc. ! queue leaky=downstream max-size-time=5000000000 ! {} ! {}. ",
        audio_proc(dst, in_audio, idx),
        mux_name);
    // muxer + sink
    pipeline_str += sink + " ";
  }
  return pipeline_str;
}

auto restream::start(const receiver_config& cfg,
                     receiver_state* state,
                     std::string& err_code,
                     std::string& err_msg) -> bool
{
  std::lock_guard<std::mutex> guard(m_pipeline_mutex);
  m_state = state;
  m_cfg = cfg;

  // Early availability check for the OUTPUT-side elements only — these don't
  // depend on the (not-yet-detected) input codec, so we can fail fast with a
  // synchronous encoder_unavailable. Input decoders are checked in phase 2,
  // once detection has revealed the real codec.
  const std::string missing = first_missing_output_element(cfg);
  if (!missing.empty()) {
    err_code = "encoder_unavailable";
    err_msg = "required GStreamer element not available on this host: " + missing;
    return false;
  }

  // Seed detection fallbacks: if tsdemux never reveals a codec, fall back to the
  // declared source codec for video and AAC for audio (legacy behaviour).
  {
    std::lock_guard<std::mutex> det(m_detect_mutex);
    m_video_found = false;
    m_in_video = cfg.in_codec;
    m_audio_found = false;
    m_in_audio = audio_codec::aac;
    m_no_more_pads = false;
  }
  m_phase.store(phase::detecting, std::memory_order_release);
  m_detect_started = std::chrono::steady_clock::now();

  return start_detection(err_code, err_msg);
}

// Phase 1: launch a throwaway pipeline that just demuxes the MPEG-TS so we can
// read the real elementary-stream codecs off the tsdemux pads. It is fed by the
// same push_buffer path as the real pipeline (shared appsrc name "videosrc").
auto restream::start_detection(std::string& err_code, std::string& err_msg)
    -> bool
{
  // Three fakesinks so up to three demuxed pads (typically 1 video + 1 audio)
  // have somewhere to drain; surplus sinks simply stay unlinked.
  const std::string det_str = std::string {k_ts_source}
      + "demux. ! fakesink sync=false async=false "
        "demux. ! fakesink sync=false async=false "
        "demux. ! fakesink sync=false async=false";

  GError* error = nullptr;
  m_pipeline = gst_parse_launch(det_str.c_str(), &error);
  if (error != nullptr) {
    err_code = "pipeline_launch_failed";
    err_msg = error->message;
    g_clear_error(&error);
    if (m_pipeline != nullptr) {
      gst_object_unref(m_pipeline);
      m_pipeline = nullptr;
    }
    return false;
  }
  if (m_pipeline == nullptr) {
    err_code = "pipeline_launch_failed";
    err_msg = "gst_parse_launch returned null (detection)";
    return false;
  }

  m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "videosrc");
  if (m_appsrc == nullptr) {
    err_code = "pipeline_launch_failed";
    err_msg = "appsrc 'videosrc' not found in detection pipeline";
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
    return false;
  }

  GstCaps* caps = gst_caps_new_simple("video/mpegts",
                                      "systemstream",
                                      G_TYPE_BOOLEAN,
                                      TRUE,
                                      "packetsize",
                                      G_TYPE_INT,
                                      k_mpeg_ts_packet_size,
                                      nullptr);
  gst_app_src_set_caps(GST_APP_SRC(m_appsrc), caps);
  gst_caps_unref(caps);

  GstElement* demux = gst_bin_get_by_name(GST_BIN(m_pipeline), "demux");
  if (demux != nullptr) {
    g_signal_connect(demux,
                     "pad-added",
                     G_CALLBACK(demux_pad_added_trampoline),
                     this);
    g_signal_connect(demux,
                     "no-more-pads",
                     G_CALLBACK(demux_no_more_pads_trampoline),
                     this);
    gst_object_unref(demux);
  }

  m_bus = gst_element_get_bus(m_pipeline);
  m_cleaned_up.store(false, std::memory_order_release);

  const GstStateChangeReturn sret =
      gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    err_code = "pipeline_launch_failed";
    err_msg = "failed to set detection pipeline to PLAYING";
    clear_pipeline_state();
    return false;
  }

  m_running.store(true, std::memory_order_release);
  m_bus_thread = std::thread([this]() -> void { bus_loop(); });
  log("Restream codec-detection pipeline PLAYING.\n");
  return true;
}

auto restream::demux_pad_added_trampoline(GstElement* /*demux*/,
                                          GstPad* pad,
                                          gpointer user_data) -> void
{
  static_cast<restream*>(user_data)->on_demux_pad_added(pad);
}

auto restream::demux_no_more_pads_trampoline(GstElement* /*demux*/,
                                             gpointer user_data) -> void
{
  auto* self = static_cast<restream*>(user_data);
  {
    std::lock_guard<std::mutex> det(self->m_detect_mutex);
    self->m_no_more_pads = true;
  }
}

auto restream::caps_event_probe(GstPad* /*pad*/,
                                GstPadProbeInfo* info,
                                gpointer user_data) -> GstPadProbeReturn
{
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (event == nullptr || GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;  // not our event: keep the probe installed
  }
  GstCaps* caps = nullptr;
  gst_event_parse_caps(event, &caps);
  if (caps != nullptr) {
    static_cast<restream*>(user_data)->record_caps(caps);
  }
  return GST_PAD_PROBE_REMOVE;  // got the caps; we're done with this pad
}

auto restream::on_demux_pad_added(GstPad* pad) -> void
{
  // Caps are usually already set from the PMT at pad-added time; if not, install
  // a one-shot probe that fires when the CAPS event arrives.
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps != nullptr) {
    record_caps(caps);
    gst_caps_unref(caps);
    return;
  }
  gst_pad_add_probe(pad,
                    GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                    caps_event_probe,
                    this,
                    nullptr);
}

auto restream::record_caps(const GstCaps* caps) -> void
{
  if (caps == nullptr || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
    return;
  }
  const GstStructure* str = gst_caps_get_structure(caps, 0);
  const char* name = gst_structure_get_name(str);
  if (name == nullptr) {
    return;
  }
  const std::string media {name};

  std::lock_guard<std::mutex> det(m_detect_mutex);
  if (media == "video/x-h264") {
    m_in_video = codec::h264;
    m_video_found = true;
  } else if (media == "video/x-h265") {
    m_in_video = codec::h265;
    m_video_found = true;
  } else if (media == "video/x-av1") {
    m_in_video = codec::av1;
    m_video_found = true;
  } else if (media == "audio/x-opus") {
    m_in_audio = audio_codec::opus;
    m_audio_found = true;
  } else if (media == "audio/x-ac3") {
    m_in_audio = audio_codec::ac3;
    m_audio_found = true;
  } else if (media == "audio/x-eac3") {
    m_in_audio = audio_codec::eac3;
    m_audio_found = true;
  } else if (media == "audio/mpeg") {
    gint mpegversion = 0;
    gst_structure_get_int(str, "mpegversion", &mpegversion);
    m_in_audio = mpegversion == 1 ? audio_codec::mp2 : audio_codec::aac;
    m_audio_found = true;
  } else {
    return;  // unrecognised media type: ignore, let detection time out
  }
  log(std::format("Detected {} stream ({}).\n",
                  media.starts_with("video") ? "video" : "audio",
                  media));
}

// Phase 2: runs on the bus/worker thread once detection has settled. Tears down
// the detection pipeline and launches the real restream pipeline built for the
// detected codecs.
auto restream::finish_detection_and_launch() -> void
{
  std::lock_guard<std::mutex> guard(m_pipeline_mutex);

  codec in_video {};
  audio_codec in_audio {};
  bool video_found = false;
  bool audio_found = false;
  {
    std::lock_guard<std::mutex> det(m_detect_mutex);
    in_video = m_in_video;
    in_audio = m_in_audio;
    video_found = m_video_found;
    audio_found = m_audio_found;
  }
  m_phase.store(phase::running, std::memory_order_release);

  log(std::format(
      "Codec detection complete: video={}{}, audio={}{}.\n",
      to_string(in_video),
      video_found ? "" : " (fallback: not detected)",
      to_string(in_audio),
      audio_found ? "" : " (fallback: assumed AAC)"));

  // Tear down the detection pipeline (without the "stopped" log / cleaned_up
  // bookkeeping clear_pipeline_state() would do — we are rebuilding, not
  // stopping).
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
  }
  if (m_appsrc != nullptr) {
    gst_object_unref(m_appsrc);
    m_appsrc = nullptr;
  }
  if (m_bus != nullptr) {
    gst_object_unref(m_bus);
    m_bus = nullptr;
  }
  if (m_pipeline != nullptr) {
    gst_object_unref(GST_OBJECT(m_pipeline));
    m_pipeline = nullptr;
  }

  auto fail = [this](const std::string& text) -> void
  {
    log(text + "\n");
    if (m_state != nullptr) {
      std::lock_guard<std::mutex> sguard(m_state->mutex);
      m_state->last_bus_error = text;
    }
  };

  // Real availability check, now that the input codecs are known.
  const std::string missing = first_missing_element(m_cfg, in_video, in_audio);
  if (!missing.empty()) {
    fail("Restream cannot start: required GStreamer element not available for "
         "the detected codec: "
         + missing);
    return;
  }

  const std::string pipeline_str =
      build_pipeline_string(m_cfg, in_video, in_audio, /*redact=*/false);
  log("Restream pipeline:\n"
      + build_pipeline_string(m_cfg, in_video, in_audio, /*redact=*/true)
      + "\n");

  GError* error = nullptr;
  m_pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
  if (error != nullptr) {
    const std::string msg = error->message;
    g_clear_error(&error);
    if (m_pipeline != nullptr) {
      gst_object_unref(m_pipeline);
      m_pipeline = nullptr;
    }
    fail("Restream pipeline launch failed: " + msg);
    return;
  }
  if (m_pipeline == nullptr) {
    fail("Restream pipeline launch failed: gst_parse_launch returned null");
    return;
  }

  m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "videosrc");
  if (m_appsrc == nullptr) {
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
    fail("Restream pipeline launch failed: appsrc 'videosrc' not found");
    return;
  }

  GstCaps* caps = gst_caps_new_simple("video/mpegts",
                                      "systemstream",
                                      G_TYPE_BOOLEAN,
                                      TRUE,
                                      "packetsize",
                                      G_TYPE_INT,
                                      k_mpeg_ts_packet_size,
                                      nullptr);
  gst_app_src_set_caps(GST_APP_SRC(m_appsrc), caps);
  gst_caps_unref(caps);

  // rtmp2sink has a known issue in some GStreamer versions where setting
  // `location` via gst_parse_launch doesn't propagate to the internal
  // connection object before the element tries to connect. Set it
  // programmatically here (after parse_launch, before PLAYING) to guarantee
  // the host is visible when the async connect fires.
  for (std::size_t idx = 0; idx < m_cfg.destinations.size(); ++idx) {
    const destination& dst = m_cfg.destinations[idx];
    if (dst.proto != output_proto::rtmp && dst.proto != output_proto::rtmps) {
      continue;
    }
    const std::string sink_name = std::format("osink{}", idx);
    GstElement* sink =
        gst_bin_get_by_name(GST_BIN(m_pipeline), sink_name.c_str());
    if (sink == nullptr) {
      continue;
    }
    const std::string loc = dst.key_or_streamid.empty()
        ? dst.url
        : std::format("{}/{}", dst.url, dst.key_or_streamid);
    g_object_set(G_OBJECT(sink), "location", loc.c_str(), nullptr);
    gst_object_unref(sink);
  }

  m_bus = gst_element_get_bus(m_pipeline);

  const GstStateChangeReturn sret =
      gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    fail("Restream pipeline failed to set PLAYING");
    clear_pipeline_state();
    return;
  }

  log("Restream pipeline PLAYING.\n");
}

auto restream::stop() -> void
{
  m_running.store(false, std::memory_order_release);
  if (m_bus_thread.joinable()) {
    m_bus_thread.join();
  }
  std::lock_guard<std::mutex> guard(m_pipeline_mutex);
  clear_pipeline_state();
  m_state = nullptr;
}

auto restream::clear_pipeline_state() -> void
{
  if (m_cleaned_up.load(std::memory_order_acquire)) {
    return;
  }
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
  }
  if (m_appsrc != nullptr) {
    gst_object_unref(m_appsrc);
    m_appsrc = nullptr;
  }
  if (m_bus != nullptr) {
    gst_object_unref(m_bus);
    m_bus = nullptr;
  }
  if (m_pipeline != nullptr) {
    gst_object_unref(GST_OBJECT(m_pipeline));
    m_pipeline = nullptr;
    log("Restream pipeline stopped.\n");
  }
  m_cleaned_up.store(true, std::memory_order_release);
}

auto restream::push_buffer(const uint8_t* data, std::size_t len) -> int
{
  if (data == nullptr || len == 0) {
    return 0;
  }
  GstElement* src = nullptr;
  {
    std::lock_guard<std::mutex> guard(m_pipeline_mutex);
    if (m_appsrc == nullptr
        || !m_running.load(std::memory_order_acquire))
    {
      return 0;  // pipeline not up / tearing down: drop, keep RIST connection
    }
    src = m_appsrc;
    gst_object_ref(src);
  }

  GstBuffer* buf = gst_buffer_new_memdup(data, len);
  const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
  gst_object_unref(src);

  if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING && ret != GST_FLOW_EOS) {
    log(std::format("appsrc push error: {}\n", gst_flow_get_name(ret)));
  }
  return 0;  // always keep the RIST connection alive
}

auto restream::bus_loop() -> void
{
  // This single worker thread spans both phases: it drives the detection ->
  // real transition and watches whichever pipeline's bus is current. The bus is
  // re-fetched every iteration because finish_detection_and_launch() swaps it.
  while (m_running.load(std::memory_order_acquire)) {
    if (m_phase.load(std::memory_order_acquire) == phase::detecting) {
      bool done = false;
      {
        std::lock_guard<std::mutex> det(m_detect_mutex);
        const bool have_both = m_video_found && m_audio_found;
        const bool timed_out =
            (std::chrono::steady_clock::now() - m_detect_started)
            >= k_detect_timeout;
        done = m_no_more_pads || have_both || timed_out;
      }
      if (done) {
        finish_detection_and_launch();
      }
    }

    GstBus* local_bus = nullptr;
    {
      std::lock_guard<std::mutex> guard(m_pipeline_mutex);
      if (m_bus != nullptr) {
        local_bus = m_bus;
        gst_object_ref(local_bus);
      }
    }
    if (local_bus == nullptr) {
      // Between phases (or phase 2 failed to launch): nothing to watch yet.
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
      continue;
    }

    GstMessage* msg = gst_bus_timed_pop_filtered(
        local_bus,
        static_cast<GstClockTime>(100 * GST_MSECOND),
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS
                                    | GST_MESSAGE_WARNING));
    gst_object_unref(local_bus);
    if (msg == nullptr) {
      continue;
    }
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        const std::string text = std::format(
            "Restream pipeline error from {}: {}",
            GST_OBJECT_NAME(msg->src),
            err != nullptr ? err->message : "(unknown)");
        log(text + "\n");
        // Don't surface detection-phase errors as the session error — that
        // pipeline is throwaway and may emit benign not-linked noise.
        if (m_state != nullptr
            && m_phase.load(std::memory_order_acquire) == phase::running)
        {
          std::lock_guard<std::mutex> guard(m_state->mutex);
          m_state->last_bus_error = text;
        }
        g_clear_error(&err);
        g_free(dbg);
        break;
      }
      case GST_MESSAGE_EOS:
        log("Restream pipeline received EOS.\n");
        break;
      case GST_MESSAGE_WARNING: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_warning(msg, &err, &dbg);
        log(std::format("Restream pipeline warning from {}: {}\n",
                        GST_OBJECT_NAME(msg->src),
                        err != nullptr ? err->message : "(unknown)"));
        g_clear_error(&err);
        g_free(dbg);
        break;
      }
      default:
        break;
    }
    gst_message_unref(msg);
  }
}
