// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>

#include <gst/gst.h>

#include "control/control.h"
#include "fanout/output.h"
#include "fanout/recorder.h"
#include "fanout/ring.h"
#include "lib/lib.h"
#include "receive/receive.h"

namespace
{
constexpr int k_default_control_port = 8080;
constexpr int k_default_rist_port = 5000;
constexpr int k_port_max = 65535;
constexpr auto k_watchdog_tick = std::chrono::seconds(1);

auto stderr_log(const std::string& msg) -> void
{
  std::fputs(msg.c_str(), stderr);
  std::fflush(stderr);
}

auto print_usage(const char* argv0) -> void
{
  std::cout
      << "open-broadcast-receiver — headless RIST receiver, copy-only fan-out\n\n"
      << "Usage: " << argv0 << " [options]\n\n"
      << "  --control-port <port>   HTTP control port (default 8080)\n"
      << "  --rist-port <port>      RIST listen port (default 5000)\n"
      << "  --bind <host>           HTTP bind address (default 127.0.0.1;\n"
      << "                          non-loopback binds REQUIRE a token or the\n"
      << "                          explicit --allow-unauthenticated flag)\n"
      << "  --token <secret>        Bearer token for the control API\n"
      << "  --allow-unauthenticated Run without a token on a non-loopback bind\n"
      << "                          (NOT for production; loud warning)\n"
      << "  --psk <hex>             librist PSK for the RIST listener\n"
      << "  --psk-aes <128|256>     PSK AES key size (default 256)\n"
      << "  --record-dir <path>     record incoming TS verbatim to\n"
      << "                          <dir>/<session_id>.ts\n"
      << "  --idle-timeout <s>      no RIST payload for this long while running\n"
      << "                          => stop with last_bus_error=idle_timeout\n"
      << "                          (default 0 = off)\n"
      << "  --egress-deny <cidrs>   comma-separated CIDR deny-list for output\n"
      << "                          destinations (hosted: node subnet+metadata)\n"
      << "  --egress-allow-private  permit RFC1918/ULA output destinations\n"
      << "                          (LAN restreaming; metadata/loopback stay\n"
      << "                          blocked)\n"
      << "  --buffer-min <ms>       RIST recovery buffer floor (default 1000)\n"
      << "  --buffer-max <ms>       RIST recovery buffer ceiling (default 5000)\n"
      << "  --rtt-min <ms>          RIST recovery RTT min (default 40)\n"
      << "  --rtt-max <ms>          RIST recovery RTT max (default 500)\n"
      << "  --reorder-buffer <ms>   RIST reorder hold-off (default 30; keep well\n"
      << "                          below buffer-min or retransmission is starved)\n"
      << "  --help                  Show this help\n\n"
      << "The receiver terminates one RIST/TS ingest and fans it out, copy-only\n"
      << "(H.264+AAC), to up to 8 RTMP/RTMPS/SRT/RIST outputs over an in-process\n"
      << "ring — one independent pipeline per output. POST /start (schema 2)\n"
      << "configures outputs; see docs/CONTRACT.md.\n";
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

auto is_loopback_bind(const std::string& host) -> bool
{
  return host == "localhost" || host == "::1" || host == "[::1]"
      || host.starts_with("127.");
}

auto split_csv(const std::string& text) -> std::vector<std::string>
{
  std::vector<std::string> out;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

auto steady_ms() -> int64_t
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The live fan-out session: ring + consumers. Owned by main, guarded by the
// lifecycle mutex. Construction: ring → recorder → outputs → RIST last (data
// only flows once every consumer exists). Teardown REVERSE of data flow:
// RIST first (joins librist workers ⇒ producer can no longer touch the ring),
// then ring close (wakes consumers), then outputs/recorder, then the ring
// itself. (TRANSPORT_PROFILE §3.6's teardown note, tightened for memory
// safety — recorded in DECISIONS.md.)
struct session
{
  std::unique_ptr<ts_ring> ring;
  std::vector<std::unique_ptr<output>> outputs;
  std::unique_ptr<recorder> rec;
};
}  // namespace

auto main(int argc, char** argv) -> int
{
  gst_init(&argc, &argv);

#ifndef NDEBUG
  // Debug builds compile the vendored librist WITHOUT -DNDEBUG, which re-arms
  // the receiver_enqueue assert (packet_time < next->packet_time). Under the
  // encoder -> rist2rist -> receiver double hop, retransmission timing can
  // violate that invariant and SIGABRT this process (see CONTRACT.md §4 and
  // open-broadcast-encoder/docs/RIST_TIMING_FINDINGS.md). timing-mode=0
  // (SOURCE) avoids the path, but only Release builds degrade gracefully on a
  // future edge case instead of aborting a live event.
  std::cerr
      << "\n*** WARNING: Debug build (NDEBUG not defined). ***\n"
      << "*** librist asserts are armed: a bad packet time ABORTS the "
         "process. ***\n"
      << "*** Use a Release build for any real stream. ***\n\n";
#endif

  int control_port = k_default_control_port;
  int rist_port = k_default_rist_port;
  // M1.1: loopback by default. Self-hosters must take an explicit, visible
  // step (token, or the scary flag) to expose the control plane.
  std::string bind_host = "127.0.0.1";
  std::string token;
  bool allow_unauthenticated = false;
  runtime_options opts;
  egress_policy egress;
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
    auto next_str = [&](std::string& dst) -> bool
    {
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for " << arg << "\n";
        return false;
      }
      dst = argv[++idx];
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
      if (!next_str(bind_host)) {
        return 2;
      }
    } else if (arg == "--token") {
      if (!next_str(token)) {
        return 2;
      }
    } else if (arg == "--allow-unauthenticated") {
      allow_unauthenticated = true;
    } else if (arg == "--psk") {
      if (!next_str(opts.psk)) {
        return 2;
      }
    } else if (arg == "--psk-aes") {
      int val = 0;
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for --psk-aes\n";
        return 2;
      }
      try {
        val = std::stoi(argv[++idx]);
      } catch (...) {
        val = 0;
      }
      if (val != 128 && val != 256) {
        std::cerr << "--psk-aes must be 128 or 256\n";
        return 2;
      }
      opts.psk_aes = val;
    } else if (arg == "--record-dir") {
      if (!next_str(opts.record_dir)) {
        return 2;
      }
    } else if (arg == "--idle-timeout") {
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for --idle-timeout\n";
        return 2;
      }
      try {
        opts.idle_timeout_s = std::stoi(argv[++idx]);
      } catch (...) {
        opts.idle_timeout_s = -1;
      }
      if (opts.idle_timeout_s < 0) {
        std::cerr << "--idle-timeout must be >= 0 seconds\n";
        return 2;
      }
    } else if (arg == "--egress-deny") {
      std::string csv;
      if (!next_str(csv)) {
        return 2;
      }
      egress.deny_cidrs = split_csv(csv);
    } else if (arg == "--egress-allow-private") {
      egress.allow_private = true;
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

  // M1.1: an empty token on a non-loopback bind is an internet-exposed
  // unauthenticated control plane — refuse to start unless the operator
  // explicitly demanded it.
  if (token.empty() && !is_loopback_bind(bind_host)) {
    if (!allow_unauthenticated) {
      std::cerr
          << "FATAL: refusing to bind the control API to " << bind_host
          << " without a token.\n"
          << "Anyone who can reach this port could redirect your stream.\n"
          << "Either pass --token <secret>, keep --bind 127.0.0.1 behind a\n"
          << "reverse proxy, or (NOT for production) pass\n"
          << "--allow-unauthenticated to accept the risk explicitly.\n";
      return 3;
    }
  }
  if (token.empty()) {
    std::cerr << "\x1b[1;31m*** NO-AUTH MODE: control API accepts "
                 "unauthenticated requests. NOT for production. ***\x1b[0m\n";
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

  std::mutex lifecycle;  // serialises start/stop; never held by RIST threads
  session sess;

  control_server control(ctx);

  // Tear down the live session. Caller holds `lifecycle`. Order per the
  // session struct comment: producer first, then wake+stop consumers.
  auto teardown = [&ctx, &sess]() -> void
  {
    ctx.receive->stop();  // joins librist workers: producer is now quiescent
    if (sess.ring) {
      sess.ring->close();  // wake all blocked consumers
    }
    for (std::unique_ptr<output>& out : sess.outputs) {
      out->stop();
    }
    sess.outputs.clear();
    if (sess.rec) {
      sess.rec->stop();
      sess.rec.reset();
    }
    sess.ring.reset();
  };

  const auto do_start = [&ctx, &lifecycle, &sess, &teardown, &egress, &opts,
                         rist_port, buffer_min, buffer_max, rtt_min, rtt_max,
                         reorder_buffer](receiver_config& cfg,
                                         std::string& err_code,
                                         std::string& err_field,
                                         std::string& err_msg,
                                         int& http_status) -> bool
  {
    std::lock_guard<std::mutex> life(lifecycle);

    // The operator-chosen --rist-port and recovery flags are authoritative
    // for the listen socket and retransmission window. Apply up front so
    // idempotency compares the *effective* config.
    cfg.ingest.rist_listen = "rist://@[::]:" + std::to_string(rist_port);
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
      // Identical /start (same session_id + config) is an idempotent retry →
      // 200 no-op. Anything else conflicts with the running session → 409.
      if (current_session == cfg.session_id && current_cfg == cfg) {
        http_status = 200;
        return true;
      }
      http_status = 409;
      err_code = "already_running";
      err_msg = "a different session is running";
      return false;
    }

    // Egress validation + IP pinning (M1.2) — after structural validation
    // (control.cpp), before anything launches.
    if (auto res = validate_and_pin_outputs(cfg, egress, default_resolver());
        !res.ok)
    {
      http_status = 400;
      err_code = res.error_code;
      err_field = res.field;
      err_msg = res.message;
      return false;
    }

    // Element preflight (§1.2): 400 element_unavailable naming the element.
    for (const output_config& out : cfg.outputs) {
      const std::string missing = output::first_missing_element(out.type);
      if (!missing.empty()) {
        http_status = 400;
        err_code = "element_unavailable";
        err_field = "outputs[].type";
        err_msg = "missing GStreamer element: " + missing;
        return false;
      }
    }

    // ---- construction: ring → recorder → outputs → RIST ----
    sess.ring = std::make_unique<ts_ring>(
        ts_ring::size_for_bandwidth(cfg.ingest.bandwidth));

    if (!opts.record_dir.empty()) {
      sess.rec = std::make_unique<recorder>(
          opts.record_dir, cfg.session_id, *sess.ring, &stderr_log);
      // Recording failure is never session-fatal (§3.7): keep the object for
      // /status visibility (active=false).
      sess.rec->start();
    }

    for (const output_config& out_cfg : cfg.outputs) {
      auto out = std::make_unique<output>(out_cfg, *sess.ring, &stderr_log);
      out->start();
      sess.outputs.push_back(std::move(out));
    }

    ts_ring* ring_ptr = sess.ring.get();
    auto push = [ring_ptr](const uint8_t* buf, std::size_t len) -> int
    {
      ring_ptr->write(buf, len);
      return 0;  // never drop the peer; never block (§3.4)
    };
    std::string rerr;
    if (!ctx.receive->start(cfg, opts, &ctx.state, push, rerr)) {
      teardown();
      http_status = 500;
      err_code = "pipeline_launch_failed";
      err_msg = "RIST receiver failed to start: " + rerr;
      return false;
    }

    {
      std::lock_guard<std::mutex> guard(ctx.state.mutex);
      ctx.state.cfg = cfg;
      ctx.state.session_id = cfg.session_id;
      ctx.state.started_at = std::chrono::steady_clock::now();
      ctx.state.last_bus_error.clear();
    }
    ctx.state.rist_bytes.store(0, std::memory_order_relaxed);
    ctx.state.last_payload_ms.store(0, std::memory_order_relaxed);
    ctx.state.is_running.store(true, std::memory_order_release);
    http_status = 200;
    return true;
  };

  const auto do_stop = [&ctx, &lifecycle, &sess, &teardown](
                           bool has_session,
                           const std::string& session_id,
                           std::string& err_code,
                           int& http_status) -> bool
  {
    std::lock_guard<std::mutex> life(lifecycle);

    if (!ctx.state.is_running.load(std::memory_order_acquire)) {
      http_status = 200;  // already stopped: no-op (§5 leniency)
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
    teardown();
    {
      std::lock_guard<std::mutex> guard(ctx.state.mutex);
      ctx.state.session_id.clear();
    }
    http_status = 200;
    return true;
  };

  // Snapshot for /status + /stats — atomics and short mutexes only; never
  // touches pipelines (§1.4 cadence rule). Holding `lifecycle` here also
  // guarantees the outputs vector is not mid-teardown.
  const auto get_stats = [&ctx, &lifecycle, &sess]() -> session_stats
  {
    std::lock_guard<std::mutex> life(lifecycle);
    session_stats snap;
    if (sess.ring) {
      snap.ring_size_bytes = sess.ring->capacity();
    }
    if (sess.rec) {
      snap.recording_active = sess.rec->active();
      snap.recording_bytes = sess.rec->bytes_written();
      snap.recording_dropped = sess.rec->dropped_bytes();
    }
    snap.rist_bytes_total =
        ctx.state.rist_bytes.load(std::memory_order_relaxed);
    snap.rist = ctx.receive->flow_stats();
    for (const std::unique_ptr<output>& out : sess.outputs) {
      output_stat ostat;
      ostat.id = out->id();
      ostat.type = to_string(out->proto());
      ostat.state = out->state_name();
      ostat.connected_s = out->connected_s();
      ostat.reconnects = out->reconnects();
      ostat.bytes_sent = out->bytes_sent();
      ostat.dropped_bytes = out->dropped_bytes();
      ostat.audio_dropped = out->audio_dropped();
      ostat.last_error = out->last_error();
      snap.outputs.push_back(std::move(ostat));
    }
    return snap;
  };

  control.set_handlers(do_start, do_stop, get_stats);

  if (!control.listen(bind_host, control_port)) {
    std::cerr << "FATAL: failed to bind control server to " << bind_host << ":"
              << control_port << "\n";
    return 1;
  }

  std::cout << "open-broadcast-receiver listening: control http://" << bind_host
            << ":" << control_port << "  rist @[::]:" << rist_port
            << (token.empty() ? "  [NO-AUTH]" : "  [token auth]")
            << (opts.psk.empty() ? "" : "  [psk]") << "\n"
            << "Copy-only fan-out (schema 2): POST /start with outputs[]. "
            << "Ctrl-C to quit." << std::endl;

  // --- idle-timeout watchdog (§1.5) -----------------------------------------
  std::atomic_bool shutting_down {false};
  std::thread watchdog(
      [&]() -> void
      {
        while (!shutting_down.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(k_watchdog_tick);
          if (opts.idle_timeout_s <= 0
              || !ctx.state.is_running.load(std::memory_order_acquire))
          {
            continue;
          }
          const int64_t last =
              ctx.state.last_payload_ms.load(std::memory_order_relaxed);
          if (last == 0) {
            continue;  // no payload yet: the pre-start TTL is the agent's job
          }
          if (steady_ms() - last
              > static_cast<int64_t>(opts.idle_timeout_s) * 1000)
          {
            std::lock_guard<std::mutex> life(lifecycle);
            if (!ctx.state.is_running.load(std::memory_order_acquire)) {
              continue;
            }
            stderr_log("idle-timeout: no RIST payload for "
                       + std::to_string(opts.idle_timeout_s)
                       + " s; stopping session\n");
            ctx.state.is_running.store(false, std::memory_order_release);
            teardown();
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            ctx.state.last_bus_error = "idle_timeout";
          }
        }
      });

  int sig = 0;
  sigwait(&sigset, &sig);
  std::cout << "\nSignal " << sig << " received; shutting down...\n";

  shutting_down.store(true, std::memory_order_release);
  watchdog.join();

  control.stop_listening();
  {
    std::lock_guard<std::mutex> life(lifecycle);
    ctx.state.is_running.store(false, std::memory_order_release);
    teardown();
  }
  ctx.receive.reset();

  return 0;
}
