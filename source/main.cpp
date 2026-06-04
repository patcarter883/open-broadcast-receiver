#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include <pthread.h>

#include <gst/gst.h>

#include "control/control.h"
#include "lib/lib.h"
#include "receive/receive.h"
#include "restream/restream.h"

namespace
{
constexpr int k_default_control_port = 8080;
constexpr int k_default_rist_port = 5000;
constexpr int k_port_max = 65535;

auto stderr_log(const std::string& msg) -> void
{
  std::fputs(msg.c_str(), stderr);
  std::fflush(stderr);
}

auto print_usage(const char* argv0) -> void
{
  std::cout
      << "open-broadcast-receiver — headless RIST receiver / restreamer\n\n"
      << "Usage: " << argv0 << " [options]\n\n"
      << "  --control-port <port>   HTTP control port (default 8080)\n"
      << "  --rist-port <port>      RIST listen port (default 5000)\n"
      << "  --bind <host>           HTTP bind address (default 0.0.0.0)\n"
      << "  --token <secret>        Bearer token for the control API\n"
      << "                          (empty => DEV no-auth mode; not for production)\n"
      << "  --buffer-min <ms>       RIST recovery buffer floor (default 1000)\n"
      << "  --buffer-max <ms>       RIST recovery buffer ceiling (default 5000)\n"
      << "  --rtt-min <ms>          RIST recovery RTT min (default 40)\n"
      << "  --rtt-max <ms>          RIST recovery RTT max (default 500)\n"
      << "  --reorder-buffer <ms>   RIST reorder hold-off (default 30; keep well\n"
      << "                          below buffer-min or retransmission is starved)\n"
      << "  --help                  Show this help\n\n"
      << "All stream control (start/stop/status) is performed over the HTTP\n"
      << "control API by open-broadcast-encoder. See docs/CONTRACT.md.\n";
}

auto parse_port(const char* str, int& out) -> bool
{
  try {
    const int val = std::stoi(str);
    if (val < 1 || val > k_port_max) {
      return false;
    }
    out = val;
    return true;
  } catch (...) {
    return false;
  }
}
}  // namespace

auto main(int argc, char** argv) -> int
{
  gst_init(&argc, &argv);

  int control_port = k_default_control_port;
  int rist_port = k_default_rist_port;
  std::string bind_host = "0.0.0.0";
  std::string token;
  // RIST recovery tuning (ms). Operator-authoritative over the /start body's
  // ingest block, like --rist-port. Defaults come from receiver_defaults.
  int buffer_min = receiver_defaults::buffer_min_ms;
  int buffer_max = receiver_defaults::buffer_max_ms;
  int rtt_min = receiver_defaults::rtt_min_ms;
  int rtt_max = receiver_defaults::rtt_max_ms;
  int reorder_buffer = receiver_defaults::reorder_buffer_ms;

  for (int idx = 1; idx < argc; ++idx) {
    const std::string_view arg = argv[idx];
    auto next = [&](int& dst) -> bool
    {
      if (idx + 1 >= argc || !parse_port(argv[idx + 1], dst)) {
        std::cerr << "Invalid or missing value for " << arg << "\n";
        return false;
      }
      ++idx;
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--control-port") {
      if (!next(control_port)) {
        return 2;
      }
    } else if (arg == "--rist-port") {
      if (!next(rist_port)) {
        return 2;
      }
    } else if (arg == "--bind") {
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for --bind\n";
        return 2;
      }
      bind_host = argv[++idx];
    } else if (arg == "--token") {
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for --token\n";
        return 2;
      }
      token = argv[++idx];
    } else if (arg == "--buffer-min") {
      if (!next(buffer_min)) {
        return 2;
      }
    } else if (arg == "--buffer-max") {
      if (!next(buffer_max)) {
        return 2;
      }
    } else if (arg == "--rtt-min") {
      if (!next(rtt_min)) {
        return 2;
      }
    } else if (arg == "--rtt-max") {
      if (!next(rtt_max)) {
        return 2;
      }
    } else if (arg == "--reorder-buffer") {
      if (!next(reorder_buffer)) {
        return 2;
      }
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  // Block SIGINT/SIGTERM in all threads; main alone waits via sigwait. Threads
  // (httplib server, librist workers) are created after this and inherit the
  // block, so only this thread services the signal — async-signal-safe.
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGINT);
  sigaddset(&sigset, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

  app_context ctx;
  ctx.auth_token = token;
  ctx.receive = std::make_unique<rist_receive>(&stderr_log);
  ctx.restreamer = std::make_unique<restream>(&stderr_log);

  std::mutex lifecycle;  // serialises start/stop; never held by bus/RIST threads

  control_server control(ctx);

  control.set_handlers(
      // --- start ---
      [&ctx, &lifecycle, rist_port, buffer_min, buffer_max, rtt_min, rtt_max,
       reorder_buffer](const receiver_config& body_cfg,
                       std::string& err_code,
                       std::string& err_msg,
                       int& http_status) -> bool
      {
        std::lock_guard<std::mutex> life(lifecycle);

        // The operator-chosen --rist-port and recovery flags are authoritative
        // for the listen socket and retransmission window. Apply up front so
        // idempotency compares the *effective* config.
        receiver_config cfg = body_cfg;
        cfg.ingest.rist_listen = std::format("rist://@[::]:{}", rist_port);
        cfg.ingest.buffer_min = buffer_min;
        cfg.ingest.buffer_max = buffer_max;
        cfg.ingest.rtt_min = rtt_min;
        cfg.ingest.rtt_max = rtt_max;
        cfg.ingest.reorder_buffer = reorder_buffer;

        if (ctx.state.is_running.load(std::memory_order_acquire)) {
          std::string current_session;
          receiver_config current_cfg;
          {
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            current_session = ctx.state.session_id;
            current_cfg = ctx.state.cfg;
          }
          if (current_session == cfg.session_id) {
            if (current_cfg == cfg) {
              http_status = 200;  // genuine idempotent retry
              return true;
            }
            http_status = 409;
            err_code = "already_running";
            err_msg = "session is running with a different configuration";
            return false;
          }
          http_status = 409;
          err_code = "already_running";
          err_msg = "receiver is already running a different session";
          return false;
        }

        if (!ctx.restreamer->start(cfg, &ctx.state, err_code, err_msg)) {
          http_status = (err_code == "encoder_unavailable") ? 400 : 500;
          return false;
        }

        std::string rerr;
        auto push = [&ctx](const uint8_t* buf, std::size_t len) -> int
        { return ctx.restreamer->push_buffer(buf, len); };
        if (!ctx.receive->start(cfg, &ctx.state, push, rerr)) {
          ctx.restreamer->stop();
          err_code = "pipeline_launch_failed";
          err_msg = "RIST receiver failed to start: " + rerr;
          http_status = 500;
          return false;
        }

        {
          std::lock_guard<std::mutex> guard(ctx.state.mutex);
          ctx.state.cfg = cfg;
          ctx.state.session_id = cfg.session_id;
          ctx.state.started_at = std::chrono::steady_clock::now();
          ctx.state.last_bus_error.clear();
        }
        ctx.state.is_running.store(true, std::memory_order_release);
        http_status = 200;
        return true;
      },
      // --- stop ---
      [&ctx, &lifecycle](bool has_session,
                         const std::string& session_id,
                         std::string& err_code,
                         int& http_status) -> bool
      {
        std::lock_guard<std::mutex> life(lifecycle);

        if (!ctx.state.is_running.load(std::memory_order_acquire)) {
          http_status = 200;  // already stopped: no-op
          return true;
        }
        if (has_session) {
          std::string current;
          {
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            current = ctx.state.session_id;
          }
          if (current != session_id) {
            http_status = 409;
            err_code = "session_mismatch";
            return false;
          }
        }

        ctx.state.is_running.store(false, std::memory_order_release);
        ctx.restreamer->stop();  // flush appsrc first so RIST thread can exit
        ctx.receive->stop();
        {
          std::lock_guard<std::mutex> guard(ctx.state.mutex);
          ctx.state.session_id.clear();
        }
        http_status = 200;
        return true;
      });

  if (!control.listen(bind_host, control_port)) {
    std::cerr << "FATAL: failed to bind control server to " << bind_host << ":"
              << control_port << "\n";
    return 1;
  }

  std::cout << "open-broadcast-receiver listening: control http://" << bind_host
            << ":" << control_port << "  rist @[::]:" << rist_port
            << (token.empty() ? "  [DEV no-auth]" : "  [token auth]") << "\n";
  std::cout << "Idle until POST /start. Ctrl-C to quit." << std::endl;

  int sig = 0;
  sigwait(&sigset, &sig);
  std::cout << "\nSignal " << sig << " received; shutting down...\n";

  control.stop_listening();
  ctx.restreamer->stop();  // flush appsrc first so RIST thread can exit
  ctx.receive->stop();
  ctx.receive.reset();
  ctx.restreamer.reset();

  return 0;
}
