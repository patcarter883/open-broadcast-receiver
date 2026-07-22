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

// Elements each template needs (TRANSPORT_PROFILE §1.2 element_unavailable).
auto required_elements(output_proto proto) -> std::vector<const char*>
{
  switch (proto) {
    case output_proto::rtmp:
    case output_proto::rtmps:
      return {"appsrc", "tsparse", "tsdemux", "queue",
              "h264parse", "aacparse", "flvmux", "rtmp2sink"};
    case output_proto::srt:
      return {"appsrc", "tsparse", "srtsink"};
    case output_proto::rist:
      return {"appsrc", "tsparse", "ristsink"};
  }
  return {};
}

auto has_property(GstElement* elem, const char* name) -> bool
{
  return g_object_class_find_property(G_OBJECT_GET_CLASS(elem), name)
      != nullptr;
}
}  // namespace

output::output(output_config cfg, ts_ring& ring, log_fn log)
    : m_cfg {std::move(cfg)}
    , m_ring {ring}
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

auto output::set_state(run_state next) -> void
{
  m_state.store(next, std::memory_order_release);
  if (next == run_state::running) {
    m_running_since_ms.store(now_ms(), std::memory_order_relaxed);
  } else {
    m_running_since_ms.store(0, std::memory_order_relaxed);
  }
}

auto output::first_missing_element(output_proto proto) -> std::string
{
  for (const char* name : required_elements(proto)) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (factory == nullptr) {
      return name;
    }
    gst_object_unref(factory);
  }
  return {};
}

// ---------------------------------------------------------------------------
// Pipeline construction (TRANSPORT_PROFILE §3.5)
// ---------------------------------------------------------------------------

auto output::build_pipeline(std::string& err) -> bool
{
  // The parse string carries NO secrets and NO customer URLs — the sink is
  // created unconfigured (name=osink) and its connect properties are set via
  // g_object_set below. That keeps pipeline strings loggable by definition.
  std::string tmpl;
  switch (m_cfg.type) {
    case output_proto::srt:
      tmpl =
          "appsrc name=osrc is-live=true do-timestamp=true format=time "
          "block=true max-bytes=4194304 "
          "! tsparse alignment=7 "
          "! srtsink name=osink wait-for-connection=false";
      break;
    case output_proto::rist:
      tmpl =
          "appsrc name=osrc is-live=true do-timestamp=true format=time "
          "block=true max-bytes=4194304 "
          "! tsparse alignment=7 "
          "! ristsink name=osink";
      break;
    case output_proto::rtmp:
    case output_proto::rtmps:
      // FLV needs AVC stream-format + raw AAC (§3.5 pitfalls 1–2); the caps
      // filters make the conversions explicit and fail loudly if impossible.
      tmpl =
          "appsrc name=osrc is-live=true do-timestamp=true format=time "
          "block=true max-bytes=4194304 "
          "! tsparse set-timestamps=true alignment=7 "
          "! tsdemux name=d "
          "d. ! queue ! h264parse config-interval=-1 "
          "! video/x-h264,stream-format=avc,alignment=au ! mux. "
          "d. ! queue ! aacparse "
          "! audio/mpeg,mpegversion=4,stream-format=raw ! mux. "
          "flvmux name=mux streamable=true latency=1000000000 "
          "! rtmp2sink name=osink async-connect=true";
      break;
  }

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
  m_consumer = m_ring.add_consumer();
  if (m_consumer < 0) {
    set_last_error("no ring consumer slot");
    set_state(run_state::error);
    return false;
  }
  m_stopping.store(false, std::memory_order_release);
  set_state(run_state::starting);
  m_thread = std::thread([this]() -> void { worker(); });
  return true;
}

auto output::stop() -> void
{
  m_stopping.store(true, std::memory_order_release);
  if (m_thread.joinable()) {
    m_thread.join();
  }
  destroy_pipeline();
  if (m_consumer >= 0) {
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
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
