#include "restream/restream.h"

#include <format>
#include <string>
#include <vector>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace
{
constexpr int k_mpeg_ts_packet_size = 188;

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
              "pa-hqmb-mode=auto ! video/x-av1 ! av1parse config-interval=1",
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
              "target-usage=1 gop-size=120 ! video/x-av1 ! av1parse "
              "config-interval=1",
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
              "preset=low-latency-hq gop-size=120 ! av1parse config-interval=1",
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

auto audio_proc(const destination& dst, std::size_t idx) -> std::string
{
  if (!dst.audio.reencode) {
    return "aacparse";
  }
  return std::format(
      "aacparse ! avdec_aac ! audioconvert ! audioresample ! "
      "avenc_aac name=audencoder{} bitrate={}",
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
      std::string sink = std::format(
          "mpegtsmux name={0} alignment=7 ! queue ! "
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
      std::string sink = std::format(
          "mpegtsmux name={0} alignment=7 ! rtpmp2tpay ! "
          "ristsink name=osink{1} address={2} port={3}",
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

auto restream::first_missing_element(const receiver_config& cfg) const -> std::string
{
  std::vector<std::string> needed;
  for (const destination& dst : cfg.destinations) {
    if (dst.video.reencode) {
      needed.emplace_back(video_decoder(dst.video.enc, cfg.in_codec));
      needed.emplace_back(video_encoder_element(dst.video.enc, dst.video.out_codec));
      if (dst.video.upscale) {
        if (dst.video.enc == encoder::nvenc) {
          needed.emplace_back("cudaconvertscale");
        } else if (dst.video.enc == encoder::qsv) {
          needed.emplace_back("vapostproc");
        }
      }
    }
    if (dst.audio.reencode) {
      needed.emplace_back("avdec_aac");
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
                                     bool redact) -> std::string
{
  // Source + per-stream source fan-out tees. Caps filters after demux select
  // the correct (dynamic) tsdemux pad without locking stream-format, so each
  // per-output parser can negotiate the muxer it feeds independently.
  std::string pipeline_str = std::format(
      "appsrc name=videosrc is-live=true do-timestamp=true format=time "
      "stream-type=0 max-bytes=4194304 block=true emit-signals=false "
      "! queue2 ! tsparse set-timestamps=true alignment=7 ! tsdemux name=demux "
      "demux. ! {} ! queue ! tee name=vsrc "
      "demux. ! audio/mpeg ! queue ! tee name=asrc ",
      video_caps(cfg.in_codec));

  for (std::size_t idx = 0; idx < cfg.destinations.size(); ++idx) {
    const destination& dst = cfg.destinations[idx];
    std::string mux_name;
    const std::string sink = output_sink(dst, idx, mux_name, redact);

    // video branch: source tee -> processing -> muxer. leaky=downstream so a
    // single stalled endpoint drops buffers on its own branch instead of
    // back-pressuring the shared tee/appsrc (and thereby the RIST ingest).
    pipeline_str += std::format("vsrc. ! queue leaky=downstream ! {} ! {}. ",
                                video_proc(dst, cfg.in_codec, idx),
                                mux_name);
    // audio branch: source tee -> processing -> muxer
    pipeline_str += std::format(
        "asrc. ! queue leaky=downstream max-size-time=5000000000 ! {} ! {}. ",
        audio_proc(dst, idx),
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

  const std::string missing = first_missing_element(cfg);
  if (!missing.empty()) {
    err_code = "encoder_unavailable";
    err_msg = "required GStreamer element not available on this host: " + missing;
    return false;
  }

  const std::string pipeline_str = build_pipeline_string(cfg, /*redact=*/false);
  // Log a secret-redacted copy (stream keys / streamids / cnames are masked).
  log("Restream pipeline:\n" + build_pipeline_string(cfg, /*redact=*/true)
      + "\n");

  GError* error = nullptr;
  m_pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
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
    err_msg = "gst_parse_launch returned null";
    return false;
  }

  m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "videosrc");
  if (m_appsrc == nullptr) {
    err_code = "pipeline_launch_failed";
    err_msg = "appsrc 'videosrc' not found in pipeline";
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

  // rtmp2sink has a known issue in some GStreamer versions where setting
  // `location` via gst_parse_launch doesn't propagate to the internal
  // connection object before the element tries to connect. Set it
  // programmatically here (after parse_launch, before PLAYING) to guarantee
  // the host is visible when the async connect fires.
  for (std::size_t idx = 0; idx < cfg.destinations.size(); ++idx) {
    const destination& dst = cfg.destinations[idx];
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
  m_cleaned_up.store(false, std::memory_order_release);

  const GstStateChangeReturn sret =
      gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    err_code = "pipeline_launch_failed";
    err_msg = "failed to set pipeline to PLAYING";
    clear_pipeline_state();
    return false;
  }

  m_running.store(true, std::memory_order_release);
  m_bus_thread = std::thread([this]() -> void { bus_loop(); });
  log("Restream pipeline PLAYING.\n");
  return true;
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
  GstBus* local_bus = nullptr;
  {
    std::lock_guard<std::mutex> guard(m_pipeline_mutex);
    if (m_bus != nullptr) {
      local_bus = m_bus;
      gst_object_ref(local_bus);
    }
  }
  if (local_bus == nullptr) {
    return;
  }

  while (m_running.load(std::memory_order_acquire)) {
    GstMessage* msg = gst_bus_timed_pop_filtered(
        local_bus,
        static_cast<GstClockTime>(100 * GST_MSECOND),
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS
                                    | GST_MESSAGE_WARNING));
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
        if (m_state != nullptr) {
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
  gst_object_unref(local_bus);
}
