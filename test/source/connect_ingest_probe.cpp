// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// connect_ingest_probe — drive the receiver's TRUNK ingest path against a real
// RIST peer, with no control plane and no GStreamer.
//
// The relay harness needs to prove a specific thing: that a receiver which
// DIALS (connect mode) pulls media through a relay's listening sender with the
// venue sender's sequence intact. That is rist_receive's job and nothing else's
// — the fan-out outputs are irrelevant to it — so this binary wires the same
// rist_receive a /start would, captures every payload to a file, and prints the
// counters. Using the shipped receive path (not a bespoke libRIST loop) is the
// point: build_connect_url, the `username=`/`password=` SRP parameters, the
// RISTNet SRP enable and the connect-mode no-bind rule are all under test.
//
// Exit 0 when it received at least one payload; 2 on a config/start failure
// (so the harness can tell "the trunk is misconfigured" from "no media").

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "lib/lib.h"
#include "receive/receive.h"

namespace
{
struct args
{
  std::string connect_url;
  std::string srp_user;
  std::string srp_pass;
  std::string out_path;
  std::string psk;
  int seconds = 6;
};

auto log_line(const std::string& msg) -> void
{
  std::fputs(msg.c_str(), stderr);
  std::fflush(stderr);
}

auto usage(const char* argv0) -> void
{
  std::fprintf(stderr,
               "usage: %s --connect-url rist://host:port --out FILE "
               "[--srp-user U --srp-pass P] [--psk HEX] [--seconds N]\n",
               argv0);
}
}  // namespace

auto main(int argc, char** argv) -> int
{
  args opts;
  for (int idx = 1; idx < argc; ++idx) {
    const std::string_view arg = argv[idx];
    const auto value = [&]() -> std::string
    {
      return idx + 1 < argc ? argv[++idx] : std::string {};
    };
    if (arg == "--connect-url") {
      opts.connect_url = value();
    } else if (arg == "--srp-user") {
      opts.srp_user = value();
    } else if (arg == "--srp-pass") {
      opts.srp_pass = value();
    } else if (arg == "--out") {
      opts.out_path = value();
    } else if (arg == "--psk") {
      opts.psk = value();
    } else if (arg == "--seconds") {
      opts.seconds = std::stoi(value());
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (opts.connect_url.empty() || opts.out_path.empty()) {
    usage(argv[0]);
    return 2;
  }

  receiver_config cfg;
  cfg.ingest.connect_url = opts.connect_url;
  cfg.ingest.srp_username = opts.srp_user;
  cfg.ingest.srp_password = opts.srp_pass;
  // A trunk sender trickles one programme's TS; the default 1000 ms buffer is
  // what the node runs, so keep it rather than loosening the test.
  const validation_result valid = validate_config(cfg);
  if (!valid.ok) {
    std::fprintf(stderr, "probe: invalid config: %s (%s)\n",
                 valid.message.c_str(), valid.field.c_str());
    return 2;
  }

  std::FILE* out = std::fopen(opts.out_path.c_str(), "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "probe: cannot open %s\n", opts.out_path.c_str());
    return 2;
  }

  std::atomic<std::uint64_t> packets {0};
  std::atomic<std::uint64_t> bytes {0};
  receiver_state state;

  rist_receive rx(&log_line);
  runtime_options ropts;
  ropts.psk = opts.psk;
  std::string err;
  const auto push = [&](const std::uint8_t* buf, std::size_t len) -> int
  {
    std::fwrite(buf, 1, len, out);
    packets.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(len, std::memory_order_relaxed);
    return 0;
  };
  if (!rx.start(cfg, ropts, &state, push, err)) {
    std::fprintf(stderr, "probe: receive start failed: %s\n", err.c_str());
    std::fclose(out);
    return 2;
  }

  std::this_thread::sleep_for(std::chrono::seconds(opts.seconds));
  const rist_flow_stat flow = rx.flow_stats();
  rx.stop();
  std::fclose(out);

  std::printf("probe: packets=%llu bytes=%llu flow_received=%llu "
              "flow_lost=%llu\n",
              static_cast<unsigned long long>(packets.load()),
              static_cast<unsigned long long>(bytes.load()),
              static_cast<unsigned long long>(flow.received_total),
              static_cast<unsigned long long>(flow.lost_total));
  std::fflush(stdout);
  return packets.load() == 0 ? 1 : 0;
}
