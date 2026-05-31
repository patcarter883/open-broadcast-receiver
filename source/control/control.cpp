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

auto get_bool(const json& obj,
              const char* key,
              bool def,
              const std::string& field_pfx) -> bool
{
  if (!obj.contains(key)) {
    return def;
  }
  const json& val = obj.at(key);
  if (!val.is_boolean()) {
    perr("invalid_schema", field_pfx + "." + key, "must be a boolean");
  }
  return val.get<bool>();
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

auto parse_start_body(const json& jbody) -> receiver_config
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
  if (cfg.schema_version != 1) {
    perr("invalid_schema",
         "schema_version",
         "unsupported schema_version (expected 1)");
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
    perr("invalid_schema", "outputs", "required array");
  }
  const json& outs = jbody.at("outputs");
  if (outs.empty()) {
    perr("invalid_schema", "outputs", "at least one output is required");
  }

  for (std::size_t idx = 0; idx < outs.size(); ++idx) {
    const json& out_item = outs.at(idx);
    const std::string pfx = "outputs[" + std::to_string(idx) + "]";
    if (!out_item.is_object()) {
      perr("invalid_schema", pfx, "must be an object");
    }

    destination dst;
    dst.id = require_string(out_item, "id", pfx + ".id");

    const std::string type = require_string(out_item, "type", pfx + ".type");
    if (!parse_output_proto(type, dst.proto)) {
      perr("bad_enum",
           pfx + ".type",
           "invalid type (expected rtmp|rtmps|srt|rist)");
    }
    dst.url = require_string(out_item, "url", pfx + ".url");
    dst.key_or_streamid = get_string(out_item, "key_or_streamid", "", pfx);

    if (out_item.contains("params")) {
      const json& params_json = out_item.at("params");
      if (!params_json.is_object()) {
        perr("invalid_schema", pfx + ".params", "must be an object");
      }
      dst.latency_ms =
          get_int(params_json, "latency_ms", dst.latency_ms, pfx + ".params");
      dst.sender_buffer =
          get_int(params_json, "sender_buffer", dst.sender_buffer, pfx + ".params");
      dst.cname = get_string(params_json, "cname", "", pfx + ".params");
    }

    if (!out_item.contains("video") || !out_item.at("video").is_object()) {
      perr("invalid_schema", pfx + ".video", "required object");
    }
    const json& video_json = out_item.at("video");
    const std::string vmode =
        require_string(video_json, "mode", pfx + ".video.mode");
    if (vmode != "copy" && vmode != "reencode") {
      perr("bad_enum", pfx + ".video.mode", "must be copy|reencode");
    }
    dst.video.reencode = (vmode == "reencode");
    dst.video.out_codec = codec_field(
        require_string(video_json, "codec", pfx + ".video.codec"),
        pfx + ".video.codec");
    if (video_json.contains("encoder")) {
      if (!video_json.at("encoder").is_string()
          || !parse_encoder(video_json.at("encoder").get<std::string>(),
                            dst.video.enc))
      {
        perr("bad_enum",
             pfx + ".video.encoder",
             "invalid encoder (expected amd|qsv|nvenc|software)");
      }
    }
    dst.video.bitrate_kbps =
        get_int(video_json, "bitrate_kbps", dst.video.bitrate_kbps, pfx + ".video");
    dst.video.upscale = get_bool(video_json, "upscale", false, pfx + ".video");
    dst.video.width =
        get_int(video_json, "width", dst.video.width, pfx + ".video");
    dst.video.height =
        get_int(video_json, "height", dst.video.height, pfx + ".video");

    if (out_item.contains("audio")) {
      const json& audio_json = out_item.at("audio");
      if (!audio_json.is_object()) {
        perr("invalid_schema", pfx + ".audio", "must be an object");
      }
      const std::string amode =
          get_string(audio_json, "mode", "copy", pfx + ".audio");
      if (amode != "copy" && amode != "reencode") {
        perr("bad_enum", pfx + ".audio.mode", "must be copy|reencode");
      }
      dst.audio.reencode = (amode == "reencode");
      const std::string acodec =
          get_string(audio_json, "codec", "aac", pfx + ".audio");
      if (acodec != "aac") {
        perr("bad_enum", pfx + ".audio.codec", "only aac is supported");
      }
      dst.audio.bitrate_kbps = get_int(audio_json,
                                        "bitrate_kbps",
                                        dst.audio.bitrate_kbps,
                                        pfx + ".audio");
    }

    cfg.destinations.push_back(std::move(dst));
  }

  return cfg;
}

auto error_body(const std::string& code,
                const std::string& field,
                const std::string& message) -> json
{
  json err_json;
  err_json["ok"] = false;
  err_json["schema_version"] = 1;
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

auto control_server::set_handlers(start_fn on_start, stop_fn on_stop) -> void
{
  m_on_start = std::move(on_start);
  m_on_stop = std::move(on_stop);
}

auto control_server::authorized(const httplib::Request& req) const -> bool
{
  if (m_ctx.auth_token.empty()) {
    return true;  // explicit dev/no-auth mode
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
  out["schema_version"] = 1;

  if (!m_ctx.state.is_running.load(std::memory_order_acquire)) {
    out["state"] = "stopped";
    out["session_id"] = nullptr;
    out["outputs"] = json::array();
    return out.dump();
  }

  std::string sid;
  std::string last_err;
  std::vector<destination> dests;
  std::chrono::steady_clock::time_point started;
  {
    std::lock_guard<std::mutex> guard(m_ctx.state.mutex);
    sid = m_ctx.state.session_id;
    last_err = m_ctx.state.last_bus_error;
    dests = m_ctx.state.cfg.destinations;
    started = m_ctx.state.started_at;
  }

  out["state"] = last_err.empty() ? "running" : "error";
  out["session_id"] = sid;
  out["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();

  json tel;
  tel["link_quality"] = m_ctx.state.link_quality.load(std::memory_order_relaxed);
  tel["worst_case_rtt_ms"] = m_ctx.state.worst_rtt.load(std::memory_order_relaxed);
  out["telemetry"] = tel;

  json arr = json::array();
  for (const destination& dst : dests) {
    json obj;
    obj["id"] = dst.id;
    obj["type"] = to_string(dst.proto);  // url / key_or_streamid are NOT exposed
    obj["state"] = last_err.empty() ? "connected" : "error";
    if (dst.video.reencode) {
      obj["bitrate_kbps"] = dst.video.bitrate_kbps;
    } else {
      obj["bitrate_kbps"] = nullptr;
    }
    obj["last_error"] = last_err.empty() ? json(nullptr) : json(last_err);
    arr.push_back(std::move(obj));
  }
  out["outputs"] = std::move(arr);
  out["last_bus_error"] = last_err.empty() ? json(nullptr) : json(last_err);
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
               try {
                 cfg = parse_start_body(json::parse(req.body));
               } catch (const parse_error& parse_err) {
                 res.status = 400;
                 res.set_content(
                     error_body(parse_err.code, parse_err.field, parse_err.message)
                         .dump(),
                     "application/json");
                 return;
               } catch (const json::exception& json_err) {
                 res.status = 400;
                 res.set_content(
                     error_body("invalid_schema",
                                "",
                                std::string("malformed JSON: ") + json_err.what())
                         .dump(),
                     "application/json");
                 return;
               }

               const validation_result val_result = validate_config(cfg);
               if (!val_result.ok) {
                 res.status = 400;
                 res.set_content(
                     error_body(val_result.error_code, val_result.field, val_result.message).dump(),
                     "application/json");
                 return;
               }

               int status = 200;
               std::string err_code;
               std::string err_msg;
               const bool started =
                   m_on_start && m_on_start(cfg, err_code, err_msg, status);
               if (started) {
                 json body;
                 body["ok"] = true;
                 body["schema_version"] = 1;
                 body["session_id"] = cfg.session_id;
                 body["state"] = "running";
                 json out_arr = json::array();
                 for (const destination& dst : cfg.destinations) {
                   json obj;
                   obj["id"] = dst.id;
                   obj["state"] = "connecting";
                   out_arr.push_back(std::move(obj));
                 }
                 body["outputs"] = std::move(out_arr);
                 res.status = 200;
                 res.set_content(body.dump(), "application/json");
               } else {
                 res.status = status;
                 json err_body = error_body(
                     err_code.empty() ? "pipeline_launch_failed" : err_code,
                     "",
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
                   // tolerate a malformed/empty stop body
                 }
               }
               int status = 200;
               std::string err_code;
               const bool stopped =
                   m_on_stop && m_on_stop(has_session, session_id, err_code, status);
               if (stopped) {
                 json body;
                 body["ok"] = true;
                 body["schema_version"] = 1;
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
