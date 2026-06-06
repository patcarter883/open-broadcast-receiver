#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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
      << "  --rtmp-location <url>   RTMP push URL for the on-GPU H264 publish\n"
      << "                          (default the datarhei blue.stream ingest)\n"
      << "  --bitrate <kbps>        on-GPU H264 encode bitrate (default 4300;\n"
      << "                          1000..60000)\n"
      << "  --upscale               insert cudascale to encode at --width x\n"
      << "                          --height (default off)\n"
      << "  --width <px>            upscale target width (default 2560; used only\n"
      << "                          with --upscale)\n"
      << "  --height <px>           upscale target height (default 1440; used\n"
      << "                          only with --upscale)\n"
      << "  --no-hw-decode          force software decode (default: prefer\n"
      << "                          NVDEC/VA/QSV when present)\n"
      << "  --help                  Show this help\n\n"
      << "The receiver decodes the incoming RIST stream, re-encodes H264 on the\n"
      << "GPU (NVENC) and pushes a single RTMP publish to a restreaming package\n"
      << "(e.g. datarhei/restreamer), which fans it out by codec copy. AAC audio\n"
      << "is passed through. The restream pipeline AUTO-STARTS on the first RIST\n"
      << "connection using the encode settings above (no POST /start required);\n"
      << "POST /start remains an optional override. See docs/CONTRACT.md.\n";
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

// Parse a bounded integer (for --bitrate/--width/--height, whose ranges exceed
// the port ceiling). Range validation against the contract limits is done by
// validate_config() before launch; here we just bound to a sane positive int.
auto parse_int(const char* str, int& out, int min_val, int max_val) -> bool
{
  try {
    const int val = std::stoi(str);
    if (val < min_val || val > max_val) {
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
  // On-GPU re-encode infrastructure (host-set; see reencode_config). The RTMP
  // push URL targets the local restreaming package (datarhei). The encode
  // settings (bitrate/upscale/dims) come from the encoder via /start; only this
  // location + the decode preference are operator-authoritative here.
  std::string rtmp_location = reencode_config {}.rtmp_location;
  bool prefer_hw_decode = true;
  // On-GPU encode settings for AUTO-START. The receiver no longer waits for the
  // encoder's /start to supply these — it auto-starts on the first RIST
  // connection using its own config. Seeded from reencode_config defaults;
  // overridable via the flags below. A later /start can still override per-body.
  int bitrate = reencode_config {}.bitrate_kbps;
  bool upscale = reencode_config {}.upscale;
  int width = reencode_config {}.width;
  int height = reencode_config {}.height;

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
    } else if (arg == "--rtmp-location") {
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for --rtmp-location\n";
        return 2;
      }
      rtmp_location = argv[++idx];
    } else if (arg == "--bitrate") {
      if (idx + 1 >= argc || !parse_int(argv[idx + 1], bitrate, 1, 1000000)) {
        std::cerr << "Invalid or missing value for --bitrate\n";
        return 2;
      }
      ++idx;
    } else if (arg == "--upscale") {
      upscale = true;
    } else if (arg == "--width") {
      if (idx + 1 >= argc || !parse_int(argv[idx + 1], width, 1, 100000)) {
        std::cerr << "Invalid or missing value for --width\n";
        return 2;
      }
      ++idx;
    } else if (arg == "--height") {
      if (idx + 1 >= argc || !parse_int(argv[idx + 1], height, 1, 100000)) {
        std::cerr << "Invalid or missing value for --height\n";
        return 2;
      }
      ++idx;
    } else if (arg == "--no-hw-decode") {
      prefer_hw_decode = false;
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

  // The single start primitive, shared by THREE callers: (1) the POST /start
  // handler (optional override), (2) the AUTO-START at process startup, and (3)
  // the reconnect supervisor. It applies the operator-authoritative CLI
  // overrides onto the supplied cfg, runs the idempotency/409 checks, then
  // launches the restream pipeline + RIST receiver. The `lifecycle` mutex makes
  // every caller mutually exclusive. Captures the CLI locals by reference (they
  // outlive every invocation; main owns them).
  std::function<bool(const receiver_config&,
                     std::string&,
                     std::string&,
                     int&)>
      do_start = [&ctx, &lifecycle, rist_port, buffer_min, buffer_max, rtt_min,
                  rtt_max, reorder_buffer, rtmp_location, prefer_hw_decode](
                     const receiver_config& body_cfg,
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
    // The RTMP push URL and decode preference are operator-authoritative
    // (CLI), like --rist-port. Applied before the idempotency compare so it
    // sees the effective config. The encode settings (bitrate/upscale/dims)
    // come from the auto cfg (CLI defaults) or a /start body.
    cfg.reencode.rtmp_location = rtmp_location;
    cfg.reencode.prefer_hw_decode = prefer_hw_decode;

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
  };

  control.set_handlers(
      // --- start (optional override; same primitive as auto-start) ---
      do_start,
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
  const std::string rtmp_redacted =
      rtmp_location.substr(0, rtmp_location.find('?'));
  std::cout << "On-GPU H264 encode -> RTMP " << rtmp_redacted << " ("
            << (prefer_hw_decode ? "hw" : "sw")
            << " decode)  audio -> AAC passthrough\n";

  // --- AUTO-START config (the receiver's own defaults + CLI) ------------------
  // The receiver no longer waits for the encoder's POST /start. It binds RIST,
  // runs codec detection and launches the on-GPU encode -> RTMP pipeline using
  // THIS config. The session_id is a sentinel ("auto") so an explicit /start
  // with a different session 409s until POST /stop (then /start) takes over.
  receiver_config auto_cfg {};
  auto_cfg.session_id = "auto";
  auto_cfg.in_codec = codec::h264;  // detection fallback if tsdemux is slow
  auto_cfg.reencode.bitrate_kbps = bitrate;
  auto_cfg.reencode.upscale = upscale;
  auto_cfg.reencode.width = width;
  auto_cfg.reencode.height = height;
  // rtmp_location / prefer_hw_decode are applied inside do_start from the CLI.

  // Fail fast on bad CLI before we touch GStreamer. do_start applies the CLI
  // ingest/rtmp overrides itself, so mirror them here for an accurate check.
  {
    receiver_config check = auto_cfg;
    check.ingest.buffer_min = buffer_min;
    check.ingest.buffer_max = buffer_max;
    check.ingest.rtt_min = rtt_min;
    check.ingest.rtt_max = rtt_max;
    check.ingest.reorder_buffer = reorder_buffer;
    check.reencode.rtmp_location = rtmp_location;
    check.reencode.prefer_hw_decode = prefer_hw_decode;
    if (const auto res = validate_config(check); !res.ok) {
      std::cerr << "FATAL: invalid configuration (" << res.error_code << " @ "
                << res.field << "): " << res.message << "\n";
      return 1;
    }
  }

  // --- reconnect supervisor ---------------------------------------------------
  // The librist worker thread fires on_reconnect on a RE-connect (a new stream
  // that may carry a different codec). It must NOT restart the pipeline inline:
  // restream::stop() joins the bus thread and would block/deadlock the RIST
  // worker. Instead it sets restart_requested and the supervisor thread below
  // services it: take `lifecycle`, restart ONLY the restream pipeline (the RIST
  // listener stays bound) which re-arms codec detection.
  std::atomic_bool restart_requested {false};
  std::atomic_bool shutting_down {false};
  std::condition_variable supervisor_cv;
  std::mutex supervisor_mutex;

  ctx.receive->set_on_reconnect(
      [&restart_requested, &supervisor_cv, &supervisor_mutex]()
      {
        {
          std::lock_guard<std::mutex> guard(supervisor_mutex);
          restart_requested.store(true, std::memory_order_release);
        }
        supervisor_cv.notify_one();
      });

  std::thread supervisor(
      [&]()
      {
        for (;;) {
          std::unique_lock<std::mutex> wait_lock(supervisor_mutex);
          supervisor_cv.wait(wait_lock,
                             [&]
                             {
                               return restart_requested.load(
                                          std::memory_order_acquire)
                                   || shutting_down.load(
                                          std::memory_order_acquire);
                             });
          if (shutting_down.load(std::memory_order_acquire)) {
            return;
          }
          restart_requested.store(false, std::memory_order_release);
          wait_lock.unlock();

          // Re-arm detection by restarting only the restream pipeline. The RIST
          // listener (ctx.receive) stays bound across the reconnect.
          std::lock_guard<std::mutex> life(lifecycle);
          if (!ctx.state.is_running.load(std::memory_order_acquire)) {
            continue;  // stopped via /stop in the meantime; nothing to restart
          }
          receiver_config running_cfg;
          {
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            running_cfg = ctx.state.cfg;
          }
          ctx.restreamer->stop();
          std::string ec, em;
          if (!ctx.restreamer->start(running_cfg, &ctx.state, ec, em)) {
            std::cerr << "reconnect restart failed: " << ec << " " << em << "\n";
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            ctx.state.last_bus_error = ec + ": " + em;
          } else {
            std::cout << "Reconnect: restream pipeline re-armed for detection.\n";
          }
        }
      });

  // Auto-start now (binds RIST + kicks detection; the real encode -> RTMP
  // pipeline launches on detection-complete without any POST /start).
  {
    std::string ec, em;
    int st = 0;
    if (!do_start(auto_cfg, ec, em, st)) {
      std::cerr << "FATAL: auto-start failed (" << ec << "): " << em << "\n";
      shutting_down.store(true, std::memory_order_release);
      supervisor_cv.notify_one();
      supervisor.join();
      control.stop_listening();
      return 1;
    }
  }

  std::cout << "Auto-started; POST /start to reconfigure. Ctrl-C to quit."
            << std::endl;

  int sig = 0;
  sigwait(&sigset, &sig);
  std::cout << "\nSignal " << sig << " received; shutting down...\n";

  shutting_down.store(true, std::memory_order_release);
  supervisor_cv.notify_one();
  supervisor.join();

  control.stop_listening();
  ctx.restreamer->stop();  // flush appsrc first so RIST thread can exit
  ctx.receive->stop();
  ctx.receive.reset();
  ctx.restreamer.reset();

  return 0;
}
