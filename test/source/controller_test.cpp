// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the standby mode controller (plan D1): the session-end policy
// and the live<->slate hysteresis. Framework-free; exit 0 = pass. No
// GStreamer, no threads required -- every decision is driven by an explicit
// clock so the windows are asserted exactly, not sampled.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "standby/controller.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// Drive the decider in 50 ms steps between `from` and `to`, with `last_live`
// held at `last_live` throughout (a feed that has stopped). Returns how many
// times it flipped.
auto sweep(standby::mode_decider& decider,
           std::int64_t from,
           std::int64_t to,
           std::int64_t last_live) -> int
{
  int flips = 0;
  for (std::int64_t now = from; now <= to; now += 50) {
    if (decider.feed(now, last_live)) {
      ++flips;
    }
  }
  return flips;
}

// Drive the decider with payload arriving on every observation, i.e. a feed
// that is genuinely live.
auto sweep_live(standby::mode_decider& decider,
                std::int64_t from,
                std::int64_t to) -> int
{
  int flips = 0;
  for (std::int64_t now = from; now <= to; now += 50) {
    if (decider.feed(now, now)) {
      ++flips;
    }
  }
  return flips;
}
}  // namespace

auto main() -> int
{
  // ---- should_end_session: standby OFF is today's --idle-timeout ----------
  {
    standby::session_end_policy off;
    off.standby_enabled = false;
    off.idle_timeout_s = 10;
    expect(!standby::should_end_session(9'000, 0, off),
           "off: no payload ever is the agent's pre-start TTL");
    expect(!standby::should_end_session(9'000, 1'000, off),
           "off: 8 s idle is inside a 10 s timeout");
    expect(standby::should_end_session(11'001, 1'000, off),
           "off: past --idle-timeout ends the session");

    off.idle_timeout_s = 0;
    expect(!standby::should_end_session(999'999, 1'000, off),
           "off: --idle-timeout 0 means the watchdog is off");

    // A clock that went backwards must never kill a session.
    expect(!standby::should_end_session(500, 1'000, off),
           "backwards clock is not an idle timeout");
  }

  // ---- should_end_session: standby ON judges LIVE media, not payload ------
  {
    standby::session_end_policy on;
    on.standby_enabled = true;
    on.max_live_gap_s = 5;
    on.idle_timeout_s = 10;  // must be ignored while the slate is armed
    expect(!standby::should_end_session(5'001, 1'000, on),
           "on: 4 s without live is well inside max_live_gap_s");
    expect(!standby::should_end_session(6'000, 1'000, on),
           "on: exactly max_live_gap_s is not yet a dead session");
    expect(standby::should_end_session(6'001, 1'000, on),
           "on: past max_live_gap_s with no live media ends the session");

    // The --idle-timeout must not fire while the slate keeps the destinations
    // fed: a 10 s idle timeout with a 30 s live-gap budget lives on.
    on.max_live_gap_s = 30;
    expect(!standby::should_end_session(11'001, 1'000, on),
           "on: --idle-timeout does NOT apply while the slate is armed");
    expect(standby::should_end_session(31'001, 1'000, on),
           "on: the live-gap budget is what ends a standby session");

    on.max_live_gap_s = 0;
    expect(!standby::should_end_session(999'999, 1'000, on),
           "on: max_live_gap_s 0 means no limit");
    expect(!standby::should_end_session(999'999, 0, on),
           "on: never-any-live is still not this watchdog's call");
  }

  // ---- hysteresis: silence past the gap puts the slate up, once -----------
  {
    standby::mode_decider decider {1500, 500};
    expect(!decider.slate(), "decider starts live");
    expect(sweep(decider, 0, 1'400, 0) == 0,
           "no switch inside live_gap_ms");
    expect(decider.feed(1'500, 0), "the gap boundary switches to the slate");
    expect(decider.slate(), "decider is on the slate after the gap");
    expect(decider.switches() == 1, "one switch into the slate");
    // The silence continues: no further switches, however long it lasts.
    expect(sweep(decider, 1'550, 60'000, 0) == 0,
           "a continuing silence switches only once");
    expect(decider.switches() == 1, "still one switch");
  }

  // ---- a 1.5 s silence with live media around it switches once each way ----
  {
    standby::mode_decider decider {1500, 500};
    // Live until t=10 s.
    expect(sweep_live(decider, 0, 10'000) == 0,
           "steady live never switches");
    // Payload stops at 10 s; observe images of that silence.
    int flips = 0;
    for (std::int64_t now = 10'050; now <= 11'550; now += 50) {
      flips += decider.feed(now, 10'000) ? 1 : 0;
    }
    expect(flips == 1 && decider.slate(), "a 1.5 s silence switches once");
    expect(decider.switches() == 1, "switch counter is 1 after the outage");
    // Payload returns and keeps arriving: hold for live_hold_ms, then switch.
    expect(sweep_live(decider, 12'050, 12'450) == 0,
           "hold not met yet: still on the slate");
    expect(decider.slate(), "still on the slate during the hold");
    expect(decider.feed(12'550, 12'550),
           "live_hold_ms of continuous payload switches back");
    expect(!decider.slate() && decider.switches() == 2,
           "back to live, second switch");
  }

  // ---- flapping shorter than the hold never switches back ----------------
  {
    standby::mode_decider decider {1500, 500, /*start_on_slate=*/true};
    // 200 ms of real payload, then a 1.6 s gap, repeatedly: the payload never
    // holds for 500 ms without an intervening gap, so the slate stays up even
    // though the silence is still short of live_gap_ms when the hold elapses.
    int flips = 0;
    std::int64_t t = 0;
    for (int round = 0; round < 8; ++round) {
      flips += sweep_live(decider, t, t + 200);
      const std::int64_t last_seen = t + 200;
      t = last_seen;
      flips += sweep(decider, t, t + 1'600, last_seen);
      t += 1'650;
    }
    expect(decider.slate(), "flapping never returns the slate to live");
    expect(decider.switches() == 0 && flips == 0,
           "no switches at all while flapping");
  }

  // ---- flapping inside the gap window is invisible to the slate ----------
  {
    standby::mode_decider decider {1500, 500};
    // Payload resumes every 300 ms: never silent long enough to leave live.
    for (std::int64_t t = 0; t <= 20'000; t += 300) {
      expect(!decider.feed(t, t), "sub-gap hiccups stay live");
    }
    expect(decider.switches() == 0, "a hiccuping link never puts the slate up");
  }

  // ---- controller: applies the decision once per change -------------------
  {
    std::atomic<std::int64_t> last_live {0};
    std::vector<bool> applied;
    standby::standby_controller ctl(
        last_live, 1500, 500,
        [&applied](bool slate) -> void { applied.push_back(slate); },
        /*slate_available=*/true,
        nullptr, [&]() -> std::int64_t { return 0; });

    // A live feed: the receive path bumps the timestamp as payload arrives.
    auto live = [&ctl, &last_live](std::int64_t now) -> void {
      last_live.store(now);
      ctl.tick(now);
    };

    ctl.tick(0);  // the opening observation: the stage started on its slate
    expect(applied.size() == 1 && applied.back() == true,
           "first tick applies the opening slate");
    expect(ctl.switches() == 0, "the opening state is not a switch");
    expect(std::string {ctl.mode_name()} == "slate", "mode is slate");

    live(1'000);
    expect(std::string {ctl.mode_name()} == "slate",
           "a returning feed holds before live");
    live(1'250);
    expect(std::string {ctl.mode_name()} == "slate", "still holding at 250 ms");
    live(1'500);  // 500 ms of continuous payload
    expect(!ctl.slate(), "controller went back to live");
    expect(std::string {ctl.mode_name()} == "live", "mode is live");
    expect(ctl.switches() == 1, "controller counted the switch");
    expect(ctl.last_live_ms_ago() == 0, "age of a fresh payload is 0");

    // Payload stops at 1.5 s and nothing follows.
    ctl.tick(3'000);  // 1500 ms after the last payload
    expect(ctl.slate(), "controller went to the slate");
    expect(ctl.switches() == 2, "controller counted the second switch");
    expect(ctl.last_live_ms_ago() == 1'500, "age is reported from its atomics");

    const std::size_t before = applied.size();
    ctl.tick(3'100);
    expect(applied.size() == before && ctl.slate(),
           "holding the slate issues no further apply");
  }

  // ---- controller: no slate available never asks for one ------------------
  {
    std::atomic<std::int64_t> last_live {0};
    std::vector<bool> applied;
    standby::standby_controller ctl(
        last_live, 1500, 500,
        [&applied](bool slate) -> void { applied.push_back(slate); },
        /*slate_available=*/false);

    last_live.store(1'000);
    ctl.tick(1'000);
    ctl.tick(60'000);  // an hour of silence with a failed slate render
    expect(!ctl.slate(), "no slate available: the controller stays live");
    expect(ctl.switches() == 0, "no slate available: no switches");
    for (bool want : applied) {
      expect(want == false, "the stage is never asked for a slate it lacks");
    }
  }

  // ---- controller: the thread starts, samples and stops -------------------
  {
    std::atomic<std::int64_t> last_live {0};
    std::atomic<int> applies {0};
    standby::standby_controller ctl(
        last_live, 1500, 500, [&applies](bool) -> void { ++applies; });
    ctl.start();
    ctl.start();  // idempotent
    ctl.stop();
    ctl.stop();
    expect(applies.load() >= 1, "start() takes one opening observation");
  }

  std::puts("controller_test: ALL OK");
  return 0;
}
