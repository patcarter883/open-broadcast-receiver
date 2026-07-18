// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_CONTROL_CONTROL_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_CONTROL_CONTROL_H

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "httplib.h"

#include "lib/lib.h"

// control_server is the REST control plane (docs/CONTRACT.md). It owns an
// httplib::Server on a background thread, authenticates every request with a
// Bearer token, parses/validates POST /start bodies into a receiver_config,
// and delegates lifecycle to handlers supplied by main. GET /status is built
// here directly from app_context state.
class control_server
{
public:
  // start handler: given a validated config, bring the receiver up. Sets
  // http_status (200/409/500/400) and, on failure, err_code/err_msg.
  using start_fn = std::function<bool(const receiver_config& cfg,
                                      std::string& err_code,
                                      std::string& err_msg,
                                      int& http_status)>;
  // stop handler: stop the running session. has_session indicates whether a
  // session_id was supplied to match against.
  using stop_fn = std::function<bool(bool has_session,
                                     const std::string& session_id,
                                     std::string& err_code,
                                     int& http_status)>;

  explicit control_server(app_context& ctx);
  ~control_server();
  control_server(const control_server&) = delete;
  auto operator=(const control_server&) -> control_server& = delete;
  control_server(control_server&&) = delete;
  auto operator=(control_server&&) -> control_server& = delete;

  auto set_handlers(start_fn on_start, stop_fn on_stop) -> void;

  // Bind + start serving on a background thread. Returns false if the bind
  // fails (port in use / not permitted).
  auto listen(const std::string& host, int port) -> bool;
  auto stop_listening() -> void;

private:
  auto authorized(const httplib::Request& req) const -> bool;
  auto setup_routes() -> void;
  auto build_status_json() const -> std::string;

  app_context& m_ctx;
  httplib::Server m_srv;
  std::thread m_thread;
  start_fn m_on_start;
  stop_fn m_on_stop;
  std::atomic_bool m_serving {false};
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_CONTROL_CONTROL_H
