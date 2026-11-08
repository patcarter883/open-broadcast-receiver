// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_CONTROL_CONTROL_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_CONTROL_CONTROL_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "httplib.h"

#include "lib/lib.h"

// control_server is the REST control plane (docs/CONTRACT.md, schema_version
// 3). It owns an httplib::Server on a background thread, authenticates every
// request with a Bearer token, parses/validates POST /start bodies into a
// receiver_config, and delegates lifecycle to handlers supplied by main.
// GET /status and the agent-facing GET /stats are built from the snapshot
// callback (atomics only — never pipeline locks).

// Parse + schema-check a POST /start body (schema_version 4). On success
// fills `cfg` and returns true. On failure returns false and fills the
// CONTRACT error code/field/message (the HTTP handler maps these onto a 400
// body). Exposed so the parser is unit-testable without a live server.
auto parse_start_body(std::string_view body,
                      receiver_config& cfg,
                      std::string& error_code,
                      std::string& error_field,
                      std::string& error_message) -> bool;

// Serialise one output_stat into the /status (stats_view == false) or /stats
// (stats_view == true) JSON object. A transcode output carries a "transcode"
// object {codec, encoder, frames_dropped}; a copy-only output omits it
// entirely. Exposed so the per-output serialisation is unit-testable without
// a live server.
auto output_stat_json(const output_stat& stat, bool stats_view) -> std::string;

// The standby/slate snapshot GET /status and GET /stats carry as a top-level
// `standby` object (CONTRACT §5, schema_version 5). Built from atomics only —
// the mode controller's counters plus the stage's stats — so reading it never
// takes a pipeline lock.
//
// `enabled` echoes the standby.enabled the RUNNING session was started with;
// `mode` is the source actually feeding the fan-out ("live" | "slate" |
// "switching", the last while the stage has not reached the request yet — a
// copy output's keyframe wait); `switches` counts live⇄slate transitions;
// `last_live_ms_ago` is the age of the last live payload; `caps_ok` says the
// slate branch negotiated the same caps as live; `stage_rebuilds` counts stage
// rebuilds; and `degraded` is the reason the slate is running below its
// request (a render failure, "geometry defaulted", "stage failed to start"),
// or an empty string when it is healthy.
struct standby_stat
{
  bool enabled = false;
  std::string mode = "live";
  std::uint64_t switches = 0;
  std::int64_t last_live_ms_ago = 0;
  bool caps_ok = true;
  std::uint64_t stage_rebuilds = 0;
  std::string degraded;
};

// Serialise the standby snapshot. Both views carry the same members: the
// object is small and a panel should not have to branch on which endpoint it
// called (a test pins that, so the two cannot silently diverge).
auto standby_stat_json(const standby_stat& stat, bool stats_view) -> std::string;

class control_server
{
public:
  // start handler: given a structurally-validated config, run egress
  // validation + element preflight and bring the session up. On failure sets
  // http_status (400/409/500) + err_code/err_field/err_msg.
  using start_fn = std::function<bool(receiver_config& cfg,
                                      std::string& err_code,
                                      std::string& err_field,
                                      std::string& err_msg,
                                      int& http_status)>;
  // stop handler: stop the running session. has_session indicates whether a
  // session_id was supplied to match against.
  using stop_fn = std::function<bool(bool has_session,
                                     const std::string& session_id,
                                     std::string& err_code,
                                     int& http_status)>;
  // Snapshot of per-output + RIST + recording counters, built from atomics.
  using stats_fn = std::function<session_stats()>;
  // Snapshot of the standby/slate state (schema_version 5), atomics only.
  using standby_fn = std::function<standby_stat()>;

  explicit control_server(app_context& ctx);
  ~control_server();
  control_server(const control_server&) = delete;
  auto operator=(const control_server&) -> control_server& = delete;
  control_server(control_server&&) = delete;
  auto operator=(control_server&&) -> control_server& = delete;

  auto set_handlers(start_fn on_start, stop_fn on_stop, stats_fn get_stats,
                    standby_fn get_standby = {}) -> void;

  // Bind + start serving on a background thread. Returns false if the bind
  // fails (port in use / not permitted).
  auto listen(const std::string& host, int port) -> bool;
  auto stop_listening() -> void;

private:
  auto authorized(const httplib::Request& req) const -> bool;
  auto setup_routes() -> void;
  auto build_status_json() const -> std::string;
  auto build_stats_json() const -> std::string;
  // The standby object (serialised), from the atomics-only snapshot callback.
  auto standby_object() const -> std::string;

  app_context& m_ctx;
  httplib::Server m_srv;
  std::thread m_thread;
  start_fn m_on_start;
  stop_fn m_on_stop;
  stats_fn m_get_stats;
  standby_fn m_get_standby;
  std::atomic_bool m_serving {false};
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_CONTROL_CONTROL_H
