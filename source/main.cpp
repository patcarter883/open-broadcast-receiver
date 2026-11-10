// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
#include "fanout/decoder.h"
#include "fanout/output.h"
#include "fanout/recorder.h"
#include "fanout/ring.h"
#include "lib/lib.h"
#include "receive/receive.h"
#include "relay/relay.h"
#include "standby/controller.h"
#include "standby/render.h"
#include "standby/stage.h"

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
      << "  --connect-url <url>     TRUNK ingest: DIAL this rist://host:port\n"
      << "                          instead of listening. Implies no listen\n"
      << "                          socket; conflicts with a non-default\n"
      << "                          --rist-port (connect mode never binds)\n"
      << "  --srp-user <user>       TRUNK ingest: SRP username for the relay's\n"
      << "                          authenticated sender leg (needs --srp-pass)\n"
      << "  --srp-pass <pass>       TRUNK ingest: SRP password (rides the RIST\n"
      << "                          URL as username=/password=, librist's only\n"
      << "                          client-side SRP channel; never logged)\n"
      << "  --relay-in <url>        RELAY mode: RIST leg this process binds and\n"
      << "                          receives on (must contain '@'). Choosing it\n"
      << "                          replaces the receiver: no control port, no\n"
      << "                          fan-out, no GStreamer — packets are pumped\n"
      << "                          to --relay-out with their sequence intact\n"
      << "  --relay-out <url>       RELAY mode: RIST leg this process binds and\n"
      << "                          sends on (must contain '@')\n"
      << "  --relay-srp <path>      RELAY mode: ristsrppasswd file authenticating\n"
      << "                          receivers of the out leg\n"
      << "  --relay-secret <hex>    RELAY mode: AES shared secret for the out leg\n"
      << "  --relay-aes <128|256>   RELAY mode: out-leg AES key size (256)\n"
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
      << "  --allow-enhanced-rtmp   permit an h265 transcode target on rtmp/\n"
      << "                          rtmps (Enhanced FLV via eflvmux; platform\n"
      << "                          eRTMP-HEVC support varies)\n"
      << "  --standby-slate         force the standby slate ON regardless of the\n"
      << "                          /start body's standby block\n"
      << "  --no-standby            force the slate OFF (operator kill switch);\n"
      << "                          wins over the body and OBR_STANDBY\n"
      << "  --standby-brand <path>  override the body's standby.brand_image\n"
      << "  --standby-assets <dir>  directory holding the shipped slate logo\n"
      << "                          (default /usr/local/share/open-broadcast)\n"
      << "  --buffer-min <ms>       RIST recovery buffer floor (default 1000)\n"
      << "  --buffer-max <ms>       RIST recovery buffer ceiling (default 5000)\n"
      << "  --rtt-min <ms>          RIST recovery RTT min (default 40)\n"
      << "  --rtt-max <ms>          RIST recovery RTT max (default 500)\n"
      << "  --reorder-buffer <ms>   RIST reorder hold-off (default 30; keep well\n"
      << "                          below buffer-min or retransmission is starved)\n"
      << "  --session-timeout <ms>  RIST peer session timeout (default 5000). A\n"
      << "                          flow takes the MAX across peers, so this governs\n"
      << "                          the receiver. librist raises it to >= 4x\n"
      << "                          keepalive and then declares a peer dead at\n"
      << "                          max(2x recovery buffer, this), so pair it with\n"
      << "                          --buffer-max to have any effect.\n"
      << "  --help                  Show this help\n\n"
      << "The receiver terminates one RIST/TS ingest and fans it out, copy-only\n"
      << "(H.264+AAC), to up to 8 RTMP/RTMPS/SRT/RIST outputs over an in-process\n"
      << "ring — one independent pipeline per output. An output may opt into\n"
      << "the transcode tier (outputs[].transcode). POST /start (schema "
      << k_schema_version << ")\n"
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
// lifecycle mutex. Construction: ring → standby (slate + ES rings + stage) →
// decoder (only for a transcode session) → outputs → recorder → RIST last (data
// only flows once every consumer exists). The decoder owns the frame_ring that
// transcode outputs pull from; copy outputs and the recorder stay on the
// ts_ring. With standby enabled the stage is the ONE demux: it publishes
// compressed AUs into es_video/es_audio, and the decoder and every output read
// those instead of the ts_ring (fanout/output.h's es_sources).
// Teardown REVERSE of data flow: RIST first (joins librist workers ⇒ producer
// can no longer touch the ring), then the mode controller (it is the only
// caller of stage::set_slate), then the stage, then ring close (wakes
// consumers), then the decoder (stops feeding, closes the frame_ring),
// outputs/recorder, then the rings themselves — the ES rings must outlive every
// consumer of them. (TRANSPORT_PROFILE §3.6's teardown note, tightened for
// memory safety — recorded in DECISIONS.md.)
struct session
{
  std::unique_ptr<ts_ring> ring;
  std::unique_ptr<es_ring> es_video;  // standby only: the stage's AU rings
  std::unique_ptr<es_ring> es_audio;
  std::unique_ptr<standby::stage> stg;  // standby only: the one demux
  std::unique_ptr<standby::standby_controller> ctl;  // standby only: live⇄slate
  std::unique_ptr<decoder> dec;  // non-null only when transcode is requested
  std::vector<std::unique_ptr<output>> outputs;
  std::unique_ptr<recorder> rec;
  // Why the slate is running below what was asked (render failure reason or
  // "geometry defaulted"). Empty is healthy; /status echoes it verbatim.
  std::string standby_degraded;
};
}  // namespace

auto main(int argc, char** argv) -> int
{
  // Relay mode must not run gst_init: it initialises the whole media stack and
  // GStreamer's own argv handling in a process that will never build a pipeline, and
  // it strips gst-specific flags from argv before our parser sees them. Detected
  // by pre-scan so the receiver path keeps gst_init exactly where it was.
  const bool relay_requested = std::any_of(
      argv + 1, argv + argc, [](const char* arg) noexcept -> bool {
        return arg != nullptr
            && (std::string_view(arg) == "--relay-in"
                || std::string_view(arg) == "--relay-out");
      });
  if (!relay_requested) {
    gst_init(&argc, &argv);
  }

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
  int session_timeout = receiver_defaults::session_timeout_ms;
  // Standby overrides (plan B3). -1 = leave the /start body's standby.enabled
  // alone; 0 = force off (--no-standby, the operator kill switch); 1 = force on
  // (--standby-slate). Applied over the body in do_start, before the
  // idempotency comparison, like --rist-port.
  int standby_force = -1;
  std::string standby_brand;  // empty = use the body's standby.brand_image
  std::string standby_assets = "/usr/local/share/open-broadcast";
  // Trunk ingest: dial this peer instead of binding. Empty = listen (default).
  std::string connect_url;
  std::string srp_user;  // SRP client pair for the trunk (relay's sender leg)
  std::string srp_pass;
  bool rist_port_explicit = false;
  // Relay mode. Any --relay-* flag takes the process out of receiver mode
  // entirely (see the branch after parsing).
  relay::relay_config relay_cfg;

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
      rist_port_explicit = true;
    } else if (arg == "--connect-url") {
      if (!next_str(connect_url)) {
        return 2;
      }
    } else if (arg == "--srp-user") {
      if (!next_str(srp_user)) {
        return 2;
      }
    } else if (arg == "--srp-pass") {
      if (!next_str(srp_pass)) {
        return 2;
      }
    } else if (arg == "--relay-in") {
      if (!next_str(relay_cfg.in_url)) {
        return 2;
      }
    } else if (arg == "--relay-out") {
      if (!next_str(relay_cfg.out_url)) {
        return 2;
      }
    } else if (arg == "--relay-srp") {
      if (!next_str(relay_cfg.srp_path)) {
        return 2;
      }
    } else if (arg == "--relay-secret") {
      if (!next_str(relay_cfg.secret)) {
        return 2;
      }
    } else if (arg == "--relay-aes") {
      int val = 0;
      if (idx + 1 >= argc) {
        std::cerr << "Missing value for --relay-aes\n";
        return 2;
      }
      try {
        val = std::stoi(argv[++idx]);
      } catch (...) {
        val = 0;
      }
      if (val != 128 && val != 256) {
        std::cerr << "--relay-aes must be 128 or 256\n";
        return 2;
      }
      relay_cfg.aes = val;
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
    } else if (arg == "--allow-enhanced-rtmp") {
      opts.allow_enhanced_rtmp = true;
    } else if (arg == "--standby-slate") {
      standby_force = 1;
    } else if (arg == "--no-standby") {
      standby_force = 0;
    } else if (arg == "--standby-brand") {
      if (!next_str(standby_brand)) {
        return 2;
      }
    } else if (arg == "--standby-assets") {
      if (!next_str(standby_assets)) {
        return 2;
      }
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
    } else if (arg == "--session-timeout") {
      if (!next(session_timeout)) {
        return 2;
      }
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  // ---- Relay mode --------------------------------------------------------
  // A relay is not a receiver with a different URL: it has no control plane, no
  // fan-out, no standby and no GStreamer, and it must survive a venue's sender
  // coming and going without a session lifecycle. Branching here keeps all of
  // that out of main's session machinery (and keeps gst_init from running in a
  // process that will never touch a pipeline).
  if (!relay_cfg.in_url.empty() || !relay_cfg.out_url.empty()) {
    std::string relay_err;
    if (!relay::validate_relay_config(relay_cfg, relay_err)) {
      std::cerr << "FATAL: " << relay_err << "\n";
      return 2;
    }
    // The flags are contradictory, not complementary: relay mode replaces the
    // receiver, so an operator who passed both gets told rather than silently
    // getting the relay (their outputs would never start).
    if (!connect_url.empty() || !srp_user.empty() || !srp_pass.empty()
        || rist_port_explicit || !token.empty()) {
      std::cerr << "FATAL: --relay-in/--relay-out replace the receiver; do not "
                   "combine them with --connect-url, --srp-user, --srp-pass, "
                   "--rist-port or --token\n";
      return 2;
    }
    std::cerr << "relay: starting " << relay_cfg.in_url << " -> "
              << relay_cfg.out_url << "\n";
    return relay::run_relay(relay_cfg, &stderr_log);
  }

  // Trunk (connect) ingest: a receiver that dials instead of binding. Refuse
  // the "both directions" config here, at the flag layer, for the same reason
  // validate_ingest does — an explicit --rist-port alongside --connect-url is a
  // leftover from the listen form, not a second leg.
  if (!connect_url.empty() && rist_port_explicit) {
    std::cerr << "FATAL: --connect-url never binds; drop --rist-port\n";
    return 2;
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
    // The controller goes before the stage: it is the only caller of
    // stage::set_slate(), so joining it first makes the stage's desired mode
    // final before the stage tears its switches down.
    if (sess.ctl) {
      sess.ctl->stop();
      sess.ctl.reset();
    }
    if (sess.stg) {
      sess.stg->stop();
    }
    if (sess.ring) {
      sess.ring->close();  // wake all blocked consumers
    }
    // The ES rings are closed, not destroyed: every consumer below still holds
    // a pointer and must be woken out of its read() before the rings go.
    if (sess.es_video) {
      sess.es_video->close();
    }
    if (sess.es_audio) {
      sess.es_audio->close();
    }
    if (sess.dec) {
      sess.dec->stop();  // stop feeding + close the frame_ring
    }
    for (std::unique_ptr<output>& out : sess.outputs) {
      out->stop();
    }
    sess.outputs.clear();
    if (sess.rec) {
      sess.rec->stop();
      sess.rec.reset();
    }
    sess.dec.reset();
    sess.stg.reset();
    sess.es_video.reset();  // no consumer exists any more at this point
    sess.es_audio.reset();
    sess.ring.reset();
    sess.standby_degraded.clear();
  };

  // Warn once per process about slate art that was asked for but is not on
  // disk. The render still proceeds: a missing logo/brand costs the slate that
  // overlay, never the render (and never a log line per session).
  bool logo_warned = false;
  bool brand_warned = false;

  const auto do_start = [&ctx, &lifecycle, &sess, &teardown, &egress, &opts,
                         &logo_warned, &brand_warned, rist_port, connect_url,
                         srp_user, srp_pass, buffer_min,
                         buffer_max, rtt_min, rtt_max, reorder_buffer,
                         session_timeout, standby_force, standby_brand,
                         standby_assets](receiver_config& cfg,
                                         std::string& err_code,
                                         std::string& err_field,
                                         std::string& err_msg,
                                         int& http_status) -> bool
  {
    std::lock_guard<std::mutex> life(lifecycle);

    // The operator-chosen --rist-port and recovery flags are authoritative
    // for the listen socket and retransmission window. Apply up front so
    // idempotency compares the *effective* config.
    if (connect_url.empty()) {
      cfg.ingest.rist_listen = "rist://@[::]:" + std::to_string(rist_port);
      cfg.ingest.connect_url.clear();
      cfg.ingest.srp_username.clear();
      cfg.ingest.srp_password.clear();
    } else {
      // Trunk ingest: dial the relay's listening sender and bind NOTHING. The
      // listen URL is emptied rather than left at its default so
      // validate_ingest's "both directions" check catches a body that tried to
      // hand us a listen URL mid-session, instead of quietly ignoring it.
      cfg.ingest.rist_listen.clear();
      cfg.ingest.connect_url = connect_url;
      cfg.ingest.srp_username = srp_user;
      cfg.ingest.srp_password = srp_pass;
    }
    cfg.ingest.buffer_min = buffer_min;
    cfg.ingest.buffer_max = buffer_max;
    cfg.ingest.rtt_min = rtt_min;
    cfg.ingest.rtt_max = rtt_max;
    cfg.ingest.reorder_buffer = reorder_buffer;
    cfg.ingest.session_timeout = session_timeout;

    // Standby overrides (plan B3) are operator-authoritative too, and applied
    // here for the same reason: idempotency must compare the effective config,
    // so re-posting an identical body with the same flags stays a 200 no-op and
    // a flag change is a genuinely different session. --no-standby wins over
    // --standby-slate when both are given only by appearing later; keep the
    // entrypoint's mapping to one of them.
    if (standby_force >= 0) {
      cfg.standby.enabled = standby_force != 0;
    }
    if (!standby_brand.empty()) {
      cfg.standby.brand_image = standby_brand;
    }

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

    // h265 -> rtmp/rtmps is gated behind --allow-enhanced-rtmp (Enhanced FLV
    // via eflvmux). Checked here, where the operator flag is known; without it
    // the request is 400 bad_enum naming outputs[i].transcode.codec.
    if (auto res = validate_transcode_targets(cfg, opts.allow_enhanced_rtmp);
        !res.ok)
    {
      http_status = 400;
      err_code = res.error_code;
      err_field = res.field;
      err_msg = res.message;
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

    // Element preflight (§1.2): 400 naming the missing element. A missing
    // transcode-chain element is transcode_unavailable on
    // outputs[i].transcode.codec; the base chain keeps element_unavailable.
    for (std::size_t idx = 0; idx < cfg.outputs.size(); ++idx) {
      const output_config& out = cfg.outputs[idx];
      const auto missing =
          output::first_missing_element(out.type, out.transcode, cfg.in_codec);
      if (!missing) {
        continue;
      }
      http_status = 400;
      err_code = missing->transcode ? "transcode_unavailable"
                                    : "element_unavailable";
      err_field = missing->transcode
          ? "outputs[" + std::to_string(idx) + "].transcode.codec"
          : "outputs[].type";
      err_msg = "missing GStreamer element: " + missing->element;
      return false;
    }

    // ---- construction: ring → decoder → outputs → recorder → RIST ----
    // A new session starts without the previous session's recorded failure.
    {
      std::lock_guard<std::mutex> guard(ctx.state.mutex);
      ctx.state.last_bus_error.clear();
    }
    sess.ring = std::make_unique<ts_ring>(
        ts_ring::size_for_bandwidth(cfg.ingest.bandwidth));

    // ---- standby (plan D2/D3) -------------------------------------------
    // With standby DISABLED this whole block is skipped and the construction
    // path below is exactly today's: no stage, no ES rings, the decoder and
    // every output read the ts_ring.
    //
    // With it enabled the shared stage becomes the session's ONE demux and
    // publishes compressed AUs into two ES rings that the decoder and every
    // output consume. The slate is rendered here, on the /start path, bounded
    // (<=5 s per pipeline) and non-fatal: a failed render still brings the
    // session up, live-only, with the reason in /status.standby.degraded.
    es_sources es {};
    if (cfg.standby.enabled) {
      const int ring_kbps =
          std::max(cfg.standby.bitrate_kbps, cfg.ingest.bandwidth);
      sess.es_video =
          std::make_unique<es_ring>(es_ring::size_for_bps(ring_kbps));
      sess.es_audio =
          std::make_unique<es_ring>(es_ring::size_for_bps(ring_kbps));

      // The slate must match the picture the destinations already negotiated.
      // A 0 anywhere means "unknown": the standby block falls back to the
      // declared source geometry, and only when THAT is unknown too does the
      // slate default to 1080p30 — recorded as degraded, because a slate at a
      // guessed geometry is a caps mismatch the stage will refuse to switch to.
      int sw = cfg.standby.width > 0 ? cfg.standby.width : cfg.source.width;
      int sh = cfg.standby.height > 0 ? cfg.standby.height : cfg.source.height;
      int sfps = cfg.standby.fps > 0 ? cfg.standby.fps : cfg.source.fps;
      if (sw <= 0 || sh <= 0 || sfps <= 0) {
        sw = 1920;
        sh = 1080;
        sfps = 30;
        sess.standby_degraded = "geometry defaulted";
        stderr_log("standby: session geometry unknown; the slate defaults to "
                   "1920x1080@30 (degraded)\n");
      }

      standby::slate_request req;
      req.codec = to_string(cfg.in_codec);
      req.width = sw;
      req.height = sh;
      req.fps = sfps;
      req.bitrate_kbps = cfg.standby.bitrate_kbps;
      req.brand_path = cfg.standby.brand_image;
      req.text = cfg.standby.text;
      // One cache directory for every session on this host: the render is
      // keyed by geometry/text/branding, so a restart of the same body and a
      // second session with the same slate reuse the cached ES files instead
      // of re-encoding them on the /start path.
      {
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec)
            / "open-broadcast-slate";
        req.out_dir = dir.string();
      }
      // The shipped mark is optional: the image may not be installed on a
      // minimal host, and a missing file must cost the slate its logo, never
      // the render. The renderer is handed the path only when it exists; the
      // absence is logged once so a typo in --standby-assets is visible.
      {
        std::error_code ec;
        const auto logo =
            std::filesystem::path(standby_assets) / "standby-logo.png";
        if (std::filesystem::is_regular_file(logo, ec)) {
          req.logo_path = logo.string();
        } else if (!logo_warned) {
          logo_warned = true;
          stderr_log("standby: no logo at " + logo.string()
                     + "; the slate renders without the mark\n");
        }
      }
      // Same contract for the per-account brand image: render_slate drops the
      // overlay when the file is absent (render.h), and the caller says so.
      if (!req.brand_path.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(req.brand_path, ec)
            && !brand_warned)
        {
          brand_warned = true;
          stderr_log("standby: brand image " + req.brand_path
                     + " is not readable; the slate renders without it\n");
        }
      }

      std::string render_err;
      const auto assets = standby::render_slate(req, render_err);
      if (!assets) {
        sess.standby_degraded =
            render_err.empty() ? "slate render failed" : render_err;
        stderr_log("standby: slate render failed (" + sess.standby_degraded
                   + "); the session runs without a slate\n");
      }

      standby::stage_config scfg;
      scfg.in_codec = to_string(cfg.in_codec);
      scfg.width = sw;
      scfg.height = sh;
      scfg.fps = sfps;
      if (assets) {
        scfg.slate_video_es = assets->video_path;
        scfg.slate_audio_es = assets->audio_path;
      }
      // A mid-GOP join only matters to a destination that copies the stream;
      // only then does the stage's video switch wait for a keyframe.
      scfg.any_copy_output = std::any_of(
          cfg.outputs.begin(),
          cfg.outputs.end(),
          [](const output_config& out) -> bool
          { return out.transcode.target == transcode_target::none; });
      scfg.live_gap_ms = cfg.standby.live_gap_ms;
      scfg.live_hold_ms = cfg.standby.live_hold_ms;
      scfg.max_gap_ms = cfg.standby.max_gap_ms;
      scfg.video_ring = sess.es_video.get();
      scfg.audio_ring = sess.es_audio.get();
      scfg.input = sess.ring.get();

      sess.stg = std::make_unique<standby::stage>(&stderr_log);
      if (!sess.stg->start(scfg)) {
        // The stage is the ES rings' only producer: without it every output
        // would starve on an empty ring. Fall back to today's path (drop the
        // rings so the outputs and the decoder read the ts_ring) and say so.
        stderr_log("standby: the shared stage failed to start; the session "
                   "falls back to the transport-stream path\n");
        sess.stg.reset();
        sess.es_video.reset();
        sess.es_audio.reset();
        if (sess.standby_degraded.empty()) {
          sess.standby_degraded = "stage failed to start";
        }
      } else {
        es.video = sess.es_video.get();
        // The audio ring is always offered: whether the ingest actually carries
        // audio is only known once the demuxer sees a pad (the stage publishes
        // nothing into it otherwise), and a slate session's audio branch must
        // stay negotiated for the live switch to be seamless. An output whose
        // ring stays empty simply feeds no audio, exactly as today's ts path
        // does for a video-only source.
        es.audio = sess.es_audio.get();
        es.video_codec = cfg.in_codec;
      }
    }

    // The shared decode stage is built ONLY when an output opts into the
    // transcode tier; a copy-only session constructs nothing here and the
    // default tier is byte-identical to before.
    const bool any_transcode = std::any_of(
        cfg.outputs.begin(),
        cfg.outputs.end(),
        [](const output_config& out) -> bool
        { return out.transcode.target != transcode_target::none; });
    if (any_transcode) {
      // A codec mismatch is session-fatal: the ingest is not what /start
      // declared, so no output's assumed codec holds. Other decode failures are
      // contained -- copy outputs still run and transcode outputs idle until the
      // session is restarted.
      sess.dec = std::make_unique<decoder>(
          *sess.ring, cfg.in_codec, &stderr_log,
          [&ctx](const std::string& reason) -> void
          {
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            if (ctx.state.last_bus_error.empty()) {
              ctx.state.last_bus_error = reason;
            }
          },
          // Standby: the video AUs the stage already demuxed and parsed. Null
          // is the ts_ring path, unchanged.
          es.video);
      sess.dec->start();
    }

    // Re-resolve hook for reconnects (DECISIONS §18/§20): the same validation
    // and egress policy as /start, applied to the live config copy so a refused
    // answer leaves the previous pin in place. A literal-IP host re-pins to
    // itself without a query.
    const auto repin = [egress](output_config& out) -> bool
    {
      receiver_config probe;
      probe.outputs.push_back(out);
      if (!validate_and_pin_outputs(probe, egress, default_resolver()).ok) {
        return false;
      }
      out.pinned_ip = probe.outputs.front().pinned_ip;
      return true;
    };

    for (const output_config& out_cfg : cfg.outputs) {
      auto out = std::make_unique<output>(
          out_cfg,
          *sess.ring,
          sess.dec ? &sess.dec->frames() : nullptr,
          &stderr_log,
          repin,
          es);
      out->start();
      sess.outputs.push_back(std::move(out));
    }

    if (!opts.record_dir.empty()) {
      sess.rec = std::make_unique<recorder>(
          opts.record_dir, cfg.session_id, *sess.ring, &stderr_log);
      // Recording failure is never session-fatal (§3.7): keep the object for
      // /status visibility (active=false).
      sess.rec->start();
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
    }
    ctx.state.rist_bytes.store(0, std::memory_order_relaxed);
    ctx.state.last_payload_ms.store(0, std::memory_order_relaxed);
    ctx.state.is_running.store(true, std::memory_order_release);

    // The slate/live decision, last: it samples last_payload_ms, which is
    // cleared above, and drives only stage::set_slate(). The stage already put
    // the slate on air in start() when it has one, so the controller's opening
    // state is the same slate -- by construction, not by a race.
    if (cfg.standby.enabled && sess.stg) {
      const bool slate_ready = sess.stg->stats().slate_active;
      standby::stage* stg = sess.stg.get();
      sess.ctl = std::make_unique<standby::standby_controller>(
          ctx.state.last_payload_ms,
          cfg.standby.live_gap_ms,
          cfg.standby.live_hold_ms,
          [stg](bool slate) -> void { stg->set_slate(slate); },
          slate_ready,
          &stderr_log);
      sess.ctl->start();
    }

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
      ostat.transcode = out->transcode();
      ostat.transcode_encoder = out->encoder_name();
      ostat.frames_dropped = out->frames_dropped();
      ostat.state = out->state_name();
      ostat.connected_s = out->connected_s();
      ostat.reconnects = out->reconnects();
      ostat.bytes_published_total = out->bytes_published_total();
      ostat.frames_published_total = out->frames_published_total();
      ostat.bytes_pushed = out->bytes_pushed();
      ostat.dropped_bytes = out->dropped_bytes();
      ostat.audio_dropped = out->audio_dropped();
      ostat.audio_frames = out->audio_frames();
      ostat.last_error = out->last_error();
      snap.outputs.push_back(std::move(ostat));
    }
    return snap;
  };

  // Standby snapshot for /status + /stats (CONTRACT §5). Atomics only: the
  // mode controller's counters and the stage's stats. `lifecycle` keeps the
  // stage/controller from being torn down under the read; no pipeline lock and
  // no GStreamer call is made, so a cadence-critical poll cannot stall a feed.
  const auto get_standby = [&ctx, &lifecycle, &sess]() -> standby_stat
  {
    standby_stat snap;
    std::lock_guard<std::mutex> life(lifecycle);
    if (!ctx.state.is_running.load(std::memory_order_acquire)) {
      return snap;  // stopped: enabled=false, mode=live, zeroed counters
    }
    {
      std::lock_guard<std::mutex> guard(ctx.state.mutex);
      snap.enabled = ctx.state.cfg.standby.enabled;
    }
    if (!snap.enabled) {
      return snap;  // standby off: today's answer, and no slate machinery
    }
    snap.degraded = sess.standby_degraded;
    if (!sess.stg) {
      return snap;  // enabled but the stage never came up: live, degraded why
    }
    const standby::stage_stats st = sess.stg->stats();
    snap.caps_ok = st.caps_ok;
    snap.stage_rebuilds = st.rebuilds;
    if (sess.ctl) {
      snap.switches = sess.ctl->switches();
      snap.last_live_ms_ago = sess.ctl->last_live_ms_ago();
      // Where the fan-out is ACTUALLY fed: the stage's selector position. While
      // the request and the position disagree the switch is mid-flight (a copy
      // output's keyframe wait), which is the "switching" the panel shows.
      const bool want_slate = sess.ctl->slate();
      if (want_slate == st.slate_active) {
        snap.mode = st.slate_active ? "slate" : "live";
      } else {
        snap.mode = "switching";
      }
    } else {
      snap.mode = st.slate_active ? "slate" : "live";
    }
    return snap;
  };

  control.set_handlers(do_start, do_stop, get_stats, get_standby);

  if (!control.listen(bind_host, control_port)) {
    std::cerr << "FATAL: failed to bind control server to " << bind_host << ":"
              << control_port << "\n";
    return 1;
  }

  std::cout << "open-broadcast-receiver listening: control http://" << bind_host
            << ":" << control_port << "  rist @[::]:" << rist_port
            << (token.empty() ? "  [NO-AUTH]" : "  [token auth]")
            << (opts.psk.empty() ? "" : "  [psk]") << "\n"
            << "Copy-only fan-out by default (schema " << k_schema_version
            << "): POST /start with "
            << "outputs[]. Ctrl-C to quit." << std::endl;

  // --- idle-timeout watchdog (§1.5) -----------------------------------------
  // The policy lives in standby::should_end_session so the two cases cannot
  // drift apart: with standby OFF this is today's --idle-timeout (no RIST
  // payload), and with standby ON it is max_live_gap_s of no LIVE media — a
  // slate-running session must not be killed for "no payload", because a slate
  // session with no payload is exactly what it is for.
  std::atomic_bool shutting_down {false};
  std::thread watchdog(
      [&]() -> void
      {
        while (!shutting_down.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(k_watchdog_tick);
          if (!ctx.state.is_running.load(std::memory_order_acquire)) {
            continue;
          }
          standby::session_end_policy policy;
          std::int64_t started_ms = 0;
          {
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            policy.standby_enabled = ctx.state.cfg.standby.enabled;
            policy.max_live_gap_s = ctx.state.cfg.standby.max_live_gap_s;
            started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             ctx.state.started_at.time_since_epoch())
                             .count();
          }
          policy.idle_timeout_s = opts.idle_timeout_s;
          if (!policy.standby_enabled && policy.idle_timeout_s <= 0) {
            continue;  // the watchdog is off
          }
          std::int64_t last =
              ctx.state.last_payload_ms.load(std::memory_order_relaxed);
          if (last == 0) {
            if (!policy.standby_enabled) {
              continue;  // no payload yet: the pre-start TTL is the agent's job
            }
            // Standby ON: a session that has never seen live media still has a
            // live-gap budget, measured from the session start.
            last = started_ms;
          }
          if (last == 0
              || !standby::should_end_session(steady_ms(), last, policy)) {
            continue;
          }
          {
            std::lock_guard<std::mutex> life(lifecycle);
            if (!ctx.state.is_running.load(std::memory_order_acquire)) {
              continue;
            }
            const std::string budget =
                policy.standby_enabled
                    ? std::to_string(policy.max_live_gap_s)
                          + " s without live media (standby max_live_gap_s)"
                    : std::to_string(policy.idle_timeout_s)
                          + " s without RIST payload (--idle-timeout)";
            stderr_log("idle-timeout: " + budget + "; stopping session\n");
            ctx.state.is_running.store(false, std::memory_order_release);
            teardown();
            std::lock_guard<std::mutex> guard(ctx.state.mutex);
            ctx.state.last_bus_error = "idle_timeout";
          }
        }
      });

  // --- transcode rate from the measured link (§4.3) -------------------------
  // One sample a second of the RIST payload counter is the rate the link is
  // actually delivering. Every transcode output re-derives its encoder target
  // from it (transcode_bitrate_policy), so the portal's metadata-derived figure
  // stops being the running rate as soon as there is a measurement. The counter
  // is cumulative and atomically bumped per payload, so no pipeline or receive
  // lock is taken here; `lifecycle` only guards the outputs vector.
  std::thread link_controller(
      [&]() -> void
      {
        uint64_t prev_bytes = 0;
        auto prev_time = std::chrono::steady_clock::now();
        while (!shutting_down.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(k_watchdog_tick);
          const auto now = std::chrono::steady_clock::now();
          const uint64_t bytes =
              ctx.state.rist_bytes.load(std::memory_order_relaxed);
          const std::chrono::duration<double> elapsed = now - prev_time;
          const uint64_t delta = bytes >= prev_bytes ? bytes - prev_bytes : 0;
          prev_bytes = bytes;
          prev_time = now;
          if (!ctx.state.is_running.load(std::memory_order_acquire)) {
            continue;
          }
          if (delta == 0 || elapsed.count() <= 0.0) {
            continue;  // no payload this window: keep the last measurement
          }
          const auto kbps = static_cast<int>(
              static_cast<double>(delta) * 8.0 / elapsed.count() / 1000.0);
          std::lock_guard<std::mutex> life(lifecycle);
          for (std::unique_ptr<output>& out : sess.outputs) {
            out->retarget_bitrate(kbps);
          }
        }
      });

  int sig = 0;
  sigwait(&sigset, &sig);
  std::cout << "\nSignal " << sig << " received; shutting down...\n";

  shutting_down.store(true, std::memory_order_release);
  watchdog.join();
  link_controller.join();

  control.stop_listening();
  {
    std::lock_guard<std::mutex> life(lifecycle);
    ctx.state.is_running.store(false, std::memory_order_release);
    teardown();
  }
  ctx.receive.reset();

  return 0;
}
