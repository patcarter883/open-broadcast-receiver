// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the RIST counter split: librist emits per-interval deltas, and
// the receiver must expose them both as a ~1 s window and as cumulative session
// totals, because a consumer that differences a window figure gets garbage.
// Also pins the /stats rist block's aliases: the bare names are the window
// figures, the `_total` names are cumulative. Framework-free; exit 0 = pass.

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "control/control.h"
#include "lib/lib.h"

using json = nlohmann::json;

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto delta(uint64_t received,
           uint64_t recovered,
           uint64_t lost,
           uint64_t missing = 0,
           uint32_t rtt = 0) -> rist_flow_delta
{
  rist_flow_delta d;
  d.received = received;
  d.recovered = recovered;
  d.recovered_one_retry = recovered / 2;
  d.lost = lost;
  d.missing = missing;
  d.worst_rtt_ms = rtt;
  return d;
}
}  // namespace

auto main() -> int
{
  // ---- window vs cumulative ------------------------------------------------
  // Three emissions inside one second accumulate into a single window; the
  // totals track the same run because nothing has closed yet.
  {
    rist_flow_accumulator acc;
    rist_flow_window win;

    expect(!acc.add(delta(100, 1, 0, 2, 10), 0, 1000, win),
           "first emission opens the window");
    expect(!acc.add(delta(200, 0, 1, 1, 40), 400, 1000, win),
           "second emission is still inside the window");
    expect(!acc.add(delta(300, 2, 0, 0, 25), 900, 1000, win),
           "third emission is still inside the window");
    expect(acc.add(delta(50, 0, 0, 0, 5), 1000, 1000, win),
           "the emission at 1 s closes the window");

    expect(win.window.received == 650, "window sums received deltas");
    expect(win.window.recovered == 3, "window sums recovered deltas");
    expect(win.window.recovered_one_retry == 1, "window sums one-retry deltas");
    expect(win.window.lost == 1, "window sums lost deltas");
    expect(win.window.missing == 0, "missing is the latest depth, not a sum");
    expect(win.window.worst_rtt_ms == 40, "window keeps the worst RTT");
    expect(win.samples == 4, "window counts its emissions");
    expect(win.received_total == 650, "first window's total equals its window");
    expect(win.lost_total == 1, "total tracks loss from the first window");

    // A second window must report only the deltas since the close, while the
    // totals keep growing.
    rist_flow_window win2;
    expect(!acc.add(delta(10, 0, 0), 1200, 1000, win2),
           "next window starts empty");
    expect(acc.add(delta(20, 3, 0), 2200, 1000, win2),
           "second window closes");
    expect(win2.window.received == 30, "second window holds only its deltas");
    expect(win2.received_total == 680, "total keeps accumulating");
    expect(win2.recovered_total == 6, "recovered total keeps accumulating");
    expect(win2.lost_total == 1, "lost total is unchanged without new loss");

    // Reset drops the totals: a new session starts at zero.
    acc.reset();
    rist_flow_window win3;
    expect(!acc.add(delta(7, 0, 0), 5000, 1000, win3),
           "after reset the first emission opens a fresh window");
    expect(acc.add(delta(0, 0, 0), 6000, 1000, win3),
           "the fresh window closes a second later");
    expect(win3.received_total == 7, "reset zeroes the running totals");
    expect(win3.window.received == 7, "reset zeroes the window");
  }

  // ---- /stats rist block ---------------------------------------------------
  {
    rist_flow_stat stat;
    stat.quality = 97.5;
    stat.rtt_ms = 41;
    stat.received_window = 900;
    stat.recovered_window = 12;
    stat.recovered_one_retry_window = 10;
    stat.lost_window = 3;
    stat.missing = 0;
    stat.received_total = 180000;
    stat.recovered_total = 208;
    stat.recovered_one_retry_total = 190;
    stat.lost_total = 2;
    stat.reordered = 44;
    stat.bandwidth_bps = 6'210'000;
    stat.retry_bandwidth_bps = 12'000;
    stat.peers.push_back(rist_peer_stat {
        .id = 1,
        .rtt_ms = 38,
        .avg_rtt_ms = 40.1,
        .received = 91000,
        .received_bytes = 119000000,
        .bandwidth_bps = 3100000});

    const json rist = json::parse(rist_stat_json(stat));
    expect(rist["received_window"] == 900, "received_window is the window");
    expect(rist["received_total"] == 180000, "received_total is cumulative");
    expect(rist["recovered_window"] == 12
               && rist["recovered_total"] == 208,
           "recovered window and total stay distinct");
    expect(rist["lost_window"] == 3 && rist["lost_total"] == 2,
           "lost window and total stay distinct");
    expect(rist["recovered_one_retry_window"] == 10
               && rist["recovered_one_retry_total"] == 190,
           "one-retry window and total stay distinct");
    // Aliases: bare names keep the value they had (the window), so an old
    // consumer's arithmetic does not silently change meaning.
    expect(rist["received"] == rist["received_window"],
           "received aliases received_window");
    expect(rist["recovered"] == rist["recovered_window"],
           "recovered aliases recovered_window");
    expect(rist["lost"] == rist["lost_window"], "lost aliases lost_window");
    expect(rist["recovered_one_retry"] == rist["recovered_one_retry_window"],
           "recovered_one_retry aliases its window field");
    expect(rist["peers"][0]["received_bytes"] == 119000000,
           "peer counters are carried through");
  }

  std::puts("rist_counter_test: ALL OK");
  return 0;
}
