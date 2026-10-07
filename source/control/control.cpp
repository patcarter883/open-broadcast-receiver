// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "control/control.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
// Thrown by the body parser; carries the CONTRACT error_code + offending field.
struct parse_error
{
  std::string code;
  std::string field;
  std::string message;
};

[[noreturn]] auto perr(std::string code,
                        std::string field,
                        std::string message) -> void
{
  throw parse_error {std::move(code), std::move(field), std::move(message)};
}

// Constant-time string comparison (auth token).
auto ct_equal(std::string_view lhs, std::string_view rhs) -> bool
{
  if (lhs.size() != rhs.size()) {
    return false;
  }
  volatile unsigned char diff = 0;
  for (std::size_t idx = 0; idx < lhs.size(); ++idx) {
    diff |= static_cast<unsigned char>(lhs[idx] ^ rhs[idx]);
  }
  return diff == 0;
}

auto get_string(const json& obj,
                const char* key,
                const std::string& def,
                const std::string& field_pfx) -> std::string
{
  if (!obj.contains(key)) {
    return def;
  }
  const json& val = obj.at(key);
  if (!val.is_string()) {
    perr("invalid_schema", field_pfx + "." + key, "must be a string");
  }
  return val.get<std::string>();
}

auto require_string(const json& obj,
                    const char* key,
                    const std::string& field) -> std::string
{
  if (!obj.contains(key) || !obj.at(key).is_string()) {
    perr("invalid_schema", field, "required string field");
  }
  return obj.at(key).get<std::string>();
}

auto get_int(const json& obj,
             const char* key,
             int def,
             const std::string& field_pfx) -> int
{
  if (!obj.contains(key)) {
    return def;
  }
  const json& val = obj.at(key);
  if (!val.is_number_integer() && !val.is_number_unsigned()) {
    perr("invalid_schema", field_pfx + "." + key, "must be an integer");
  }
  return val.get<int>();
}

auto codec_field(const std::string& str,
                 const std::string& field) -> codec
{
  codec cod {};
  if (!parse_codec(str, cod)) {
    perr("bad_enum", field, "invalid codec (expected h264|h265|av1)");
  }
  return cod;
}

// Optional outputs[].transcode.codec — same shape as codec_field, but the
// transcode target has no "av1"/"none" value (copy-only = omit the object).
auto transcode_field(const std::string& str,
                     const std::string& field) -> transcode_target
{
  transcode_target target {};
  if (!parse_transcode_target(str, target)) {
    perr("bad_enum", field, "invalid transcode codec (expected h264|h265)");
  }
  return target;
}

// POST /start body — schema_version 3 (TRANSPORT_PROFILE §1.1). outputs[] is
// required but MAY be empty (a link-test / record-only session).
auto parse_start_body_json(const json& jbody) -> receiver_config
{
  if (!jbody.is_object()) {
    perr("invalid_schema", "", "request body must be a JSON object");
  }

  receiver_config cfg;

  if (!jbody.contains("schema_version")
      || !jbody.at("schema_version").is_number_integer())
  {
    perr("invalid_schema", "schema_version", "required integer field");
  }
  cfg.schema_version = jbody.at("schema_version").get<int>();
  if (cfg.schema_version != k_schema_version) {
    perr("invalid_schema",
         "schema_version",
         "unsupported schema_version (expected 3)");
  }

  cfg.session_id = require_string(jbody, "session_id", "session_id");

  if (jbody.contains("ingest")) {
    const json& ingest_json = jbody.at("ingest");
    if (!ingest_json.is_object()) {
      perr("invalid_schema", "ingest", "must be an object");
    }
    cfg.ingest.rist_listen =
        get_string(ingest_json, "rist_listen", cfg.ingest.rist_listen, "ingest");
    cfg.ingest.bandwidth =
        get_int(ingest_json, "bandwidth", cfg.ingest.bandwidth, "ingest");
    cfg.ingest.buffer_min =
        get_int(ingest_json, "buffer_min", cfg.ingest.buffer_min, "ingest");
    cfg.ingest.buffer_max =
        get_int(ingest_json, "buffer_max", cfg.ingest.buffer_max, "ingest");
    cfg.ingest.rtt_min =
        get_int(ingest_json, "rtt_min", cfg.ingest.rtt_min, "ingest");
    cfg.ingest.rtt_max =
        get_int(ingest_json, "rtt_max", cfg.ingest.rtt_max, "ingest");
    cfg.ingest.reorder_buffer =
        get_int(ingest_json, "reorder_buffer", cfg.ingest.reorder_buffer, "ingest");
  }

  if (!jbody.contains("source") || !jbody.at("source").is_object()) {
    perr("invalid_schema", "source", "required object");
  }
  cfg.in_codec = codec_field(
      require_string(jbody.at("source"), "codec", "source.codec"),
      "source.codec");

  if (!jbody.contains("outputs") || !jbody.at("outputs").is_array()) {
    perr("invalid_schema", "outputs", "required array (may be empty)");
  }
  std::size_t idx = 0;
  for (const json& out_json : jbody.at("outputs")) {
    const std::string field = "outputs[" + std::to_string(idx) + "]";
    if (!out_json.is_object()) {
      perr("invalid_schema", field, "must be an object");
    }
    output_config out;
    out.id = require_string(out_json, "id", field + ".id");
    const std::string type_str =
        require_string(out_json, "type", field + ".type");
    if (!parse_output_proto(type_str, out.type)) {
      perr("bad_enum",
           field + ".type",
           "invalid type (expected rtmp|rtmps|srt|rist)");
    }
    out.url = require_string(out_json, "url", field + ".url");
    out.key_or_streamid =
        get_string(out_json, "key_or_streamid", "", field);
    // Opt-in per-output transcode (schema_version 3). Absent => copy-only.
    if (out_json.contains("transcode")) {
      const json& tr_json = out_json.at("transcode");
      if (!tr_json.is_object()) {
        perr("invalid_schema", field + ".transcode", "must be an object");
      }
      out.transcode.target = transcode_field(
          require_string(tr_json, "codec", field + ".transcode.codec"),
          field + ".transcode.codec");
      out.transcode.bitrate_kbps =
          get_int(tr_json, "bitrate_kbps", 0, field + ".transcode");
      out.transcode.gop = get_int(tr_json, "gop", 0, field + ".transcode");
      // schema_version 4: optional output size. Parsed here, validated as a set
      // in validate_transcode_targets (scale needs a target, and both axes).
      if (tr_json.contains("scale")) {
        const json& sc_json = tr_json.at("scale");
        if (!sc_json.is_object()) {
          perr("invalid_schema",
               field + ".transcode.scale",
               "must be an object");
        }
        out.transcode.scale_width =
            get_int(sc_json, "width", 0, field + ".transcode.scale");
        out.transcode.scale_height =
            get_int(sc_json, "height", 0, field + ".transcode.scale");
      }
    }
    cfg.outputs.push_back(std::move(out));
    ++idx;
  }

  return cfg;
}

auto error_body(const std::string& code,
                const std::string& field,
                const std::string& message) -> json
{
  json err_json;
  err_json["ok"] = false;
  err_json["schema_version"] = k_schema_version;
  err_json["error_code"] = code;
  if (!field.empty()) {
    err_json["field"] = field;
  }
  err_json["message"] = message;
  return err_json;
}

constexpr std::size_t k_max_body_bytes = 256 * 1024;
}  // namespace

// ---------------------------------------------------------------------------
// /start body parsing (public: unit-testable without a live server)
// ---------------------------------------------------------------------------

auto parse_start_body(std::string_view body,
                      receiver_config& cfg,
                      std::string& error_code,
                      std::string& error_field,
                      std::string& error_message) -> bool
{
  try {
    cfg = parse_start_body_json(json::parse(body));
  } catch (const parse_error& err) {
    error_code = err.code;
    error_field = err.field;
    error_message = err.message;
    return false;
  } catch (const json::exception& json_err) {
    error_code = "invalid_schema";
    error_field.clear();
    error_message = std::string("malformed JSON: ") + json_err.what();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Per-output JSON (public: unit-testable without a live server)
// ---------------------------------------------------------------------------

auto output_stat_json(const output_stat& stat, bool stats_view) -> std::string
{
  json obj;
  obj["id"] = stat.id;
  if (!stats_view) {
    obj["type"] = stat.type;
  }
  obj["state"] = stat.state;
  if (!stats_view) {
    if (stat.state == "running") {
      obj["connected_s"] = stat.connected_s;
    }
    obj["reconnects"] = stat.reconnects;
    obj["audio_dropped"] = stat.audio_dropped;
  } else {
    obj["bytes_sent"] = stat.bytes_sent;
    obj["dropped_bytes"] = stat.dropped_bytes;
    obj["reconnects"] = stat.reconnects;
  }
  // Copy-only outputs omit the field entirely; a transcode output advertises
  // its target codec, chosen encoder element and whole-frame drop count
  // (never the source codec or any secret).
  if (stat.transcode != transcode_target::none) {
    json tr;
    tr["codec"] = to_string(stat.transcode);
    tr["encoder"] = stat.transcode_encoder;
    tr["frames_dropped"] = stat.frames_dropped;
    obj["transcode"] = std::move(tr);
  }
  if (!stats_view && !stat.last_error.empty()) {
    obj["last_error"] = stat.last_error;
  }
  return obj.dump();
}

// ---------------------------------------------------------------------------
// control_server implementations
// ---------------------------------------------------------------------------

control_server::control_server(app_context& ctx)
    : m_ctx {ctx}
{
}

control_server::~control_server()
{
  stop_listening();
}

auto control_server::set_handlers(start_fn on_start,
                                  stop_fn on_stop,
                                  stats_fn get_stats) -> void
{
  m_on_start = std::move(on_start);
  m_on_stop = std::move(on_stop);
  m_get_stats = std::move(get_stats);
}

auto control_server::authorized(const httplib::Request& req) const -> bool
{
  if (m_ctx.auth_token.empty()) {
    return true;  // explicit dev/no-auth mode (M1.1 gates this at startup)
  }
  const std::string hdr = req.get_header_value("Authorization");
  constexpr std::string_view prefix = "Bearer ";
  if (hdr.size() <= prefix.size() || !hdr.starts_with(prefix)) {
    return false;
  }
  return ct_equal(std::string_view {hdr}.substr(prefix.size()), m_ctx.auth_token);
}

auto control_server::build_status_json() const -> std::string
{
  json out;
  out["ok"] = true;
  out["schema_version"] = k_schema_version;

  if (!m_ctx.state.is_running.load(std::memory_order_acquire)) {
    // A session that ended abnormally (e.g. idle_timeout, §1.5) reports
    // state=error with the cause until the next /start.
    std::string last_err;
    std::string sid;
    {
      std::lock_guard<std::mutex> guard(m_ctx.state.mutex);
      last_err = m_ctx.state.last_bus_error;
      sid = m_ctx.state.session_id;
    }
    out["state"] = last_err.empty() ? "stopped" : "error";
    out["session_id"] = sid.empty() ? json(nullptr) : json(sid);
    out["outputs"] = json::array();
    out["last_bus_error"] = last_err.empty() ? json(nullptr) : json(last_err);
    return out.dump();
  }

  std::string sid;
  std::string last_err;
  std::chrono::steady_clock::time_point started;
  {
    std::lock_guard<std::mutex> guard(m_ctx.state.mutex);
    sid = m_ctx.state.session_id;
    last_err = m_ctx.state.last_bus_error;
    started = m_ctx.state.started_at;
  }

  // Session state stays "running" while the session is intact — per-output
  // failure is never session-fatal (§1.3). "error" is reserved for
  // session-level failures (idle_timeout, RIST listener death).
  out["state"] = last_err.empty() ? "running" : "error";
  out["session_id"] = sid;
  out["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();

  json tel;
  tel["link_quality"] = m_ctx.state.link_quality.load(std::memory_order_relaxed);
  tel["worst_case_rtt_ms"] = m_ctx.state.worst_rtt.load(std::memory_order_relaxed);
  out["telemetry"] = tel;

  const session_stats snap = m_get_stats ? m_get_stats() : session_stats {};
  json outs = json::array();
  for (const output_stat& ostat : snap.outputs) {
    outs.push_back(json::parse(output_stat_json(ostat, false)));
  }
  out["outputs"] = std::move(outs);

  json rec;
  rec["active"] = snap.recording_active;
  rec["bytes"] = snap.recording_bytes;
  out["recording"] = std::move(rec);

  out["last_bus_error"] = last_err.empty() ? json(nullptr) : json(last_err);
  return out.dump();
}

auto control_server::build_stats_json() const -> std::string
{
  json out;
  out["ok"] = true;
  out["schema_version"] = k_schema_version;
  {
    std::lock_guard<std::mutex> guard(m_ctx.state.mutex);
    out["session_id"] = m_ctx.state.session_id;
  }
  out["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();

  const session_stats snap = m_get_stats ? m_get_stats() : session_stats {};
  out["ring_size_bytes"] = snap.ring_size_bytes;

  json rist;
  rist["quality"] = snap.rist.quality;
  rist["rtt_ms"] = snap.rist.rtt_ms;
  rist["received"] = snap.rist.received;
  rist["missing"] = snap.rist.missing;
  rist["recovered"] = snap.rist.recovered;
  rist["recovered_one_retry"] = snap.rist.recovered_one_retry;
  rist["lost"] = snap.rist.lost;
  rist["reordered"] = snap.rist.reordered;
  rist["bandwidth_bps"] = snap.rist.bandwidth_bps;
  rist["retry_bandwidth_bps"] = snap.rist.retry_bandwidth_bps;
  json peers = json::array();
  for (const rist_peer_stat& peer : snap.rist.peers) {
    json pobj;
    pobj["id"] = peer.id;
    pobj["rtt_ms"] = peer.rtt_ms;
    pobj["avg_rtt_ms"] = peer.avg_rtt_ms;
    pobj["received"] = peer.received;
    pobj["received_bytes"] = peer.received_bytes;
    pobj["bandwidth_bps"] = peer.bandwidth_bps;
    peers.push_back(std::move(pobj));
  }
  rist["peers"] = std::move(peers);
  out["rist"] = std::move(rist);

  json ts_in;
  ts_in["bytes_total"] = snap.rist_bytes_total;  // agent derives bitrate
  out["ts_in"] = std::move(ts_in);

  json outs = json::array();
  for (const output_stat& ostat : snap.outputs) {
    outs.push_back(json::parse(output_stat_json(ostat, true)));
  }
  out["outputs"] = std::move(outs);

  if (snap.recording_active || snap.recording_bytes > 0) {
    json rec;
    rec["active"] = snap.recording_active;
    rec["bytes"] = snap.recording_bytes;
    rec["dropped_bytes"] = snap.recording_dropped;
    out["recording"] = std::move(rec);
  }
  return out.dump();
}

auto control_server::setup_routes() -> void
{
  m_srv.set_payload_max_length(k_max_body_bytes);
  m_srv.set_read_timeout(5, 0);
  m_srv.set_write_timeout(5, 0);
  m_srv.set_keep_alive_timeout(5);
  m_srv.set_keep_alive_max_count(5);

  m_srv.set_pre_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res)
          -> httplib::Server::HandlerResponse
      {
        if (!authorized(req)) {
          res.status = 401;
          res.set_content(R"({"ok":false,"error_code":"unauthorized"})",
                          "application/json");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  m_srv.Post("/start",
             [this](const httplib::Request& req, httplib::Response& res) -> void
             {
               receiver_config cfg;
               std::string parse_code;
               std::string parse_field;
               std::string parse_msg;
               if (!parse_start_body(
                       req.body, cfg, parse_code, parse_field, parse_msg))
               {
                 res.status = 400;
                 res.set_content(
                     error_body(parse_code, parse_field, parse_msg).dump(),
                     "application/json");
                 return;
               }

               const validation_result val_result = validate_config(cfg);
               if (!val_result.ok) {
                 // rtmp_codec_unsupported is a 400 at /start (fail early on
                 // the declared hint, §1.2); all structural errors are 400s.
                 res.status = 400;
                 res.set_content(
                     error_body(val_result.error_code,
                                val_result.field,
                                val_result.message)
                         .dump(),
                     "application/json");
                 return;
               }

               int status = 200;
               std::string err_code;
               std::string err_field;
               std::string err_msg;
               const bool started = m_on_start
                   && m_on_start(cfg, err_code, err_field, err_msg, status);
               if (started) {
                 json body;
                 body["ok"] = true;
                 body["schema_version"] = k_schema_version;
                 body["session_id"] = cfg.session_id;
                 body["state"] = "running";
                 // Echo outputs as id/type only — never URLs, never keys.
                 json outs = json::array();
                 for (const output_config& out : cfg.outputs) {
                   json obj;
                   obj["id"] = out.id;
                   obj["type"] = to_string(out.type);
                   outs.push_back(std::move(obj));
                 }
                 body["outputs"] = std::move(outs);
                 res.status = 200;
                 res.set_content(body.dump(), "application/json");
               } else {
                 res.status = status;
                 json err_body = error_body(
                     err_code.empty() ? "pipeline_launch_failed" : err_code,
                     err_field,
                     err_msg);
                 if (status == 409) {
                   std::lock_guard<std::mutex> guard(m_ctx.state.mutex);
                   err_body["session_id"] = m_ctx.state.session_id;
                 }
                 res.set_content(err_body.dump(), "application/json");
               }
             });

  m_srv.Post("/stop",
             [this](const httplib::Request& req, httplib::Response& res) -> void
             {
               bool has_session = false;
               std::string session_id;
               if (!req.body.empty()) {
                 try {
                   const json jbody = json::parse(req.body);
                   if (jbody.contains("session_id")
                       && jbody.at("session_id").is_string())
                   {
                     has_session = true;
                     session_id = jbody.at("session_id").get<std::string>();
                   }
                 } catch (const json::exception&) {
                   // tolerate a malformed/empty stop body (§5 leniency)
                 }
               }
               int status = 200;
               std::string err_code;
               const bool stopped =
                   m_on_stop && m_on_stop(has_session, session_id, err_code, status);
               if (stopped) {
                 json body;
                 body["ok"] = true;
                 body["schema_version"] = k_schema_version;
                 body["state"] = "stopped";
                 res.status = 200;
                 res.set_content(body.dump(), "application/json");
               } else {
                 res.status = status;
                 json err_body = error_body(
                     err_code.empty() ? "session_mismatch" : err_code,
                     "",
                     "stop rejected");
                 {
                   std::lock_guard<std::mutex> guard(m_ctx.state.mutex);
                   err_body["session_id"] = m_ctx.state.session_id;
                 }
                 res.set_content(err_body.dump(), "application/json");
               }
             });

  m_srv.Get("/status",
            [this](const httplib::Request& /*req*/, httplib::Response& res) -> void
            {
              res.status = 200;
              res.set_content(build_status_json(), "application/json");
            });

  m_srv.Get("/stats",
            [this](const httplib::Request& /*req*/, httplib::Response& res) -> void
            {
              res.status = 200;
              res.set_content(build_stats_json(), "application/json");
            });

  m_srv.Get("/healthz",
            [](const httplib::Request& /*req*/, httplib::Response& res) -> void
            {
              res.status = 200;
              res.set_content(R"({"ok":true})", "application/json");
            });
}

auto control_server::listen(const std::string& host, int port) -> bool
{
  setup_routes();
  if (!m_srv.bind_to_port(host, port)) {
    return false;
  }
  m_serving.store(true, std::memory_order_release);
  m_thread = std::thread([this]() -> void { m_srv.listen_after_bind(); });
  return true;
}

auto control_server::stop_listening() -> void
{
  if (m_serving.exchange(false, std::memory_order_acq_rel)) {
    m_srv.stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }
}
