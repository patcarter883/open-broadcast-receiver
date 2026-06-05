#include "restream/restream.h"

#include <chrono>
#include <cstdint>
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

// True if a GStreamer element factory of this name exists in the registry.
auto element_present(const char* name) noexcept -> bool
{
  GstElementFactory* fac = gst_element_factory_find(name);
  if (fac != nullptr) {
    gst_object_unref(fac);
    return true;
  }
  return false;
}

// Memory domain a decoder's frames live in — determines how we bring them back
// to system memory for the v4l2 device.
enum class dec_domain : std::uint8_t
{
  sys,   // libav software decode: already system memory
  cuda,  // NVDEC: CUDA device memory
  va     // VA-API / QSV: VA surface / DMABuf
};

struct video_dec
{
  std::string element;
  dec_domain domain;
};

// Pick the best available decoder for the detected codec. When prefer_hw, probe
// the registry for NVDEC, then VA-API, then QSV (all GPU); otherwise — or when
// none is present — fall back to the libav software decoder.
auto choose_video_decoder(codec cod, bool prefer_hw) -> video_dec
{
  if (prefer_hw) {
    const char* nv = cod == codec::h264 ? "nvh264dec"
        : cod == codec::h265            ? "nvh265dec"
                                        : "nvav1dec";
    if (element_present(nv)) {
      return {.element = nv, .domain = dec_domain::cuda};
    }
    const char* vaapi = cod == codec::h264 ? "vah264dec"
        : cod == codec::h265               ? "vah265dec"
                                           : "vaav1dec";
    if (element_present(vaapi)) {
      return {.element = vaapi, .domain = dec_domain::va};
    }
    const char* qsv = cod == codec::h264 ? "qsvh264dec"
        : cod == codec::h265             ? "qsvh265dec"
                                         : "qsvav1dec";
    if (element_present(qsv)) {
      return {.element = qsv, .domain = dec_domain::va};
    }
  }
  const char* sw = cod == codec::h264 ? "avdec_h264"
      : cod == codec::h265            ? "avdec_h265"
                                      : "av1dec";
  return {.element = sw, .domain = dec_domain::sys};
}

// The extra element a GPU decode domain needs to reach system memory (used by
// the registry availability check); empty for software decode.
auto download_element(dec_domain domain) noexcept -> const char*
{
  switch (domain) {
    case dec_domain::cuda:
      return "cudadownload";
    case dec_domain::va:
      return "vapostproc";
    case dec_domain::sys:
    default:
      return "";
  }
}

// The fragment that turns a decoder's output into plain system-memory raw video
// at `pix` (e.g. NV12), ready for v4l2sink. GPU domains need an explicit
// download; software decode just needs a (no-op for matching formats)
// videoconvert.
auto raw_download(dec_domain domain, const std::string& pix) -> std::string
{
  switch (domain) {
    case dec_domain::cuda:
      return std::format("cudadownload ! videoconvert ! video/x-raw,format={}",
                         pix);
    case dec_domain::va:
      return std::format("vapostproc ! video/x-raw,format={}", pix);
    case dec_domain::sys:
    default:
      return std::format("videoconvert ! video/x-raw,format={}", pix);
  }
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

// The audio branch always DECODES to PCM (uncompressed) for the raw handoff:
// <parse> ! <decoder> ! audioconvert ! audioresample ! S16LE/48k stereo caps.
// The detected codec selects the parser + decoder; audioconvert/audioresample
// normalise to the device's fixed format regardless of the source layout/rate.
auto audio_decode_fragment(audio_codec in_audio) -> std::string
{
  return std::format(
      "{} ! {} ! audioconvert ! audioresample ! "
      "audio/x-raw,format=S16LE,channels=2,rate=48000",
      audio_parse(in_audio),
      audio_decoder(in_audio));
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
  // Decode-only handoff: the video decoder (+ its GPU->system download element)
  // and the audio parser/decoder depend on the detected input codecs; the
  // convert/resample and the two device sinks are codec-independent.
  const video_dec vdec =
      choose_video_decoder(in_video, cfg.sink.prefer_hw_decode);
  std::vector<std::string> needed;
  needed.emplace_back(vdec.element);
  if (const char* dl = download_element(vdec.domain); dl[0] != '\0') {
    needed.emplace_back(dl);
  }
  needed.emplace_back(video_parse(in_video));
  needed.emplace_back(audio_parse(in_audio));
  needed.emplace_back(audio_decoder(in_audio));
  needed.insert(needed.end(),
                {"videoconvert", "audioconvert", "audioresample", "v4l2sink",
                 "alsasink"});
  for (const std::string& name : needed) {
    if (!element_present(name.c_str())) {
      return name;
    }
  }
  return {};
}

auto restream::first_missing_output_element(const receiver_config& /*cfg*/) const
    -> std::string
{
  // Codec-independent sink elements, checkable synchronously before detection.
  // The decoder (input-codec dependent) is validated in phase 2.
  for (const char* name :
       {"v4l2sink", "alsasink", "videoconvert", "audioconvert", "audioresample"})
  {
    if (!element_present(name)) {
      return name;
    }
  }
  return {};
}

auto restream::build_pipeline_string(const receiver_config& cfg,
                                     codec in_video,
                                     audio_codec in_audio) -> std::string
{
  const video_dec vdec =
      choose_video_decoder(in_video, cfg.sink.prefer_hw_decode);
  const std::string vdownload = raw_download(vdec.domain, cfg.sink.pixel_format);

  // Single consumer per stream (no tee). A caps filter after demux selects the
  // correct (dynamic) tsdemux pad; the caps come from runtime detection
  // (start_detection), not the declared config. Video: parse -> decode ->
  // system-memory raw -> v4l2loopback. Audio: parse -> decode -> PCM ->
  // snd-aloop. A bounded leaky=downstream queue before each sink drops frames if
  // the loopback device stalls instead of back-pressuring the shared
  // demux/appsrc (and thereby the RIST ingest). Both sinks sync to the pipeline
  // clock so the two devices stay time-aligned for the downstream consumer.
  return std::format(
      "{0}"
      // NB: device values are NOT single-quoted — gst_parse_launch does not
      // strip single quotes around v4l2sink/alsasink `device=` (the literal
      // quotes end up in the device name and the open fails). The values are
      // already validated quote/space-free by is_pipeline_safe (validate_sink),
      // and contain no gst-special chars, so bare interpolation is safe.
      "demux. ! {1} ! queue ! {2} ! {3} ! {4} ! "
      "queue leaky=downstream max-size-buffers=4 ! "
      "v4l2sink name=vsink device={5} sync=true "
      "demux. ! {6} ! queue ! {7} ! "
      "queue leaky=downstream max-size-time=200000000 ! "
      "alsasink name=asink device={8} sync=true ",
      k_ts_source,                      // 0
      video_caps(in_video),             // 1
      video_parse(in_video),            // 2
      vdec.element,                     // 3
      vdownload,                        // 4: <download> ! videoconvert ! caps
      cfg.sink.video_device,            // 5
      audio_caps(in_audio),             // 6
      audio_decode_fragment(in_audio),  // 7: parse ! dec ! convert ! caps
      cfg.sink.audio_device);           // 8
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

  // Publish the detected codecs for GET /status (read-only mirror).
  if (m_state != nullptr) {
    std::lock_guard<std::mutex> sguard(m_state->mutex);
    m_state->detected_video = to_string(in_video);
    m_state->detected_audio = to_string(in_audio);
  }

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
      build_pipeline_string(m_cfg, in_video, in_audio);
  log("Restream pipeline:\n" + pipeline_str + "\n");

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
            "Restream pipeline error from {}: {}{}",
            GST_OBJECT_NAME(msg->src),
            err != nullptr ? err->message : "(unknown)",
            dbg != nullptr ? std::format(" [{}]", dbg) : "");
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
