// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "standby/controller.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace standby
{
namespace
{
auto steady_ms() -> std::int64_t
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

auto should_end_session(std::int64_t now_ms,
                        std::int64_t last_live_ms,
                        const session_end_policy& policy) -> bool
{
  // Never any live media: the pre-start TTL belongs to the agent, and this is
  // today's behaviour for a receiver that has not been sent a stream yet.
  if (last_live_ms == 0) {
    return false;
  }
  const std::int64_t silent_ms = now_ms - last_live_ms;
  if (silent_ms < 0) {
    return false;  // clock went backwards: never a reason to kill a session
  }
  if (policy.standby_enabled) {
    if (policy.max_live_gap_s <= 0) {
      return false;  // 0 = no limit while the slate is on air
    }
    return silent_ms > static_cast<std::int64_t>(policy.max_live_gap_s) * 1000;
  }
  if (policy.idle_timeout_s <= 0) {
    return false;  // --idle-timeout 0 = the watchdog is off
  }
  return silent_ms > static_cast<std::int64_t>(policy.idle_timeout_s) * 1000;
}

// ---------------------------------------------------------------------------
// mode_decider
// ---------------------------------------------------------------------------

mode_decider::mode_decider(int live_gap_ms, int live_hold_ms, bool start_on_slate)
    : m_live_gap_ms {live_gap_ms}
    , m_live_hold_ms {live_hold_ms}
    , m_slate {start_on_slate}
{
}

auto mode_decider::feed(std::int64_t now_ms, std::int64_t last_live_ms) -> bool
{
  if (!m_have_epoch) {
    m_have_epoch = true;
    m_epoch_ms = now_ms;
  }
  // With no payload ever, the silence is measured from the first observation.
  const std::int64_t reference = last_live_ms > 0 ? last_live_ms : m_epoch_ms;
  const std::int64_t silent_ms =
      now_ms > reference ? now_ms - reference : 0;
  // "Currently live" is a window, not an event: payload that stopped less than
  // live_gap_ms ago still counts as live, which is what makes a link that
  // hiccups for 200 ms invisible to the slate.
  const bool live_holding = last_live_ms > 0 && silent_ms < gap_ms();

  const bool before = m_slate;
  if (!m_slate) {
    if (!live_holding && silent_ms >= gap_ms()) {
      m_slate = true;
      m_live_run = false;
    }
  } else if (live_holding) {
    if (!m_live_run) {
      // The payload resumed at `last_live_ms`, not at this observation: the
      // hold window is media time, and starting it at the sample would make
      // the switch back late by however often we sample.
      m_live_run = true;
      m_live_since_ms = last_live_ms;
    }
    // Two conditions, and both matter:
    //  - the run is at least live_hold_ms long, and
    //  - the payload is STILL arriving at that moment. Without the second, a
    //    feed that stops 200 ms after it returns would be declared live at
    //    hold (the silence has not reached live_gap_ms yet, so the run looks
    //    unbroken) and the slate would be dropped for a dead input.
    //
    // Freshness is a couple of samples, capped at a quarter of the gap window
    // and at 250 ms: payload absent for that long is not arriving, whatever
    // the hold says. The consequence is deliberate and documented: a feed
    // slower than ~4 fps cannot be distinguished from a stopped one inside a
    // hold window, and the controller then keeps the slate up (the safe side)
    // until live_gap_ms proves the feed is back.
    const std::int64_t fresh_ms = std::max<std::int64_t>(
        2 * k_sample_ms, std::min<std::int64_t>(gap_ms() / 4, 250));
    const std::int64_t age_ms = now_ms - last_live_ms;
    if (now_ms - m_live_since_ms >= hold_ms()
        && (hold_ms() == 0 || age_ms <= fresh_ms)) {
      m_slate = false;
      m_live_run = false;
    }
  } else {
    // The returning feed is not holding (a gap reappeared): forget the run.
    m_live_run = false;
  }

  if (m_slate == before) {
    return false;
  }
  ++m_switches;
  return true;
}

// ---------------------------------------------------------------------------
// standby_controller
// ---------------------------------------------------------------------------

standby_controller::standby_controller(
    const std::atomic<std::int64_t>& last_live_ms,
    int live_gap_ms,
    int live_hold_ms,
    apply_fn apply,
    bool slate_available,
    log_fn log,
    clock_fn clock)
    : m_last_live_ms {last_live_ms}
    // A session whose stage started on a slate starts here on one too -- the
    // decider must not disagree with the stage's opening set_slate on its first
    // tick. A session with no usable slate never enters slate state at all.
    , m_decider {live_gap_ms, live_hold_ms, slate_available}
    , m_apply {std::move(apply)}
    , m_slate_available {slate_available}
    , m_log {std::move(log)}
    , m_clock {std::move(clock)}
{
}

standby_controller::~standby_controller()
{
  stop();
}

auto standby_controller::start() -> void
{
  if (m_running.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  m_stopping.store(false, std::memory_order_release);
  // The first observation is taken here, before the thread exists, so a session
  // that starts with the slate already on air does not race its own opening
  // decision (and the stage's opening set_slate).
  tick(m_clock ? m_clock() : steady_ms());
  m_thread = std::thread([this]() -> void { run(); });
}

auto standby_controller::stop() -> void
{
  m_stopping.store(true, std::memory_order_release);
  if (m_thread.joinable()) {
    m_thread.join();
  }
  m_running.store(false, std::memory_order_release);
}

auto standby_controller::run() -> void
{
  while (!m_stopping.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(k_tick_ms));
    if (m_stopping.load(std::memory_order_acquire)) {
      break;
    }
    tick(m_clock ? m_clock() : steady_ms());
  }
}

auto standby_controller::tick(std::int64_t now_ms) -> void
{
  const std::int64_t last_live =
      m_last_live_ms.load(std::memory_order_relaxed);
  m_last_live_ms_ago.store(last_live > 0 && now_ms > last_live
                               ? now_ms - last_live
                               : 0,
                           std::memory_order_relaxed);

  if (!m_slate_available) {
    // No slate to switch to: report live and never ask the stage for one. The
    // first observation still hands the stage its (live) opening state.
    if (!m_applied.exchange(true, std::memory_order_acq_rel)) {
      if (m_apply) {
        m_apply(false);
      }
    }
    return;
  }

  const bool changed = m_decider.feed(now_ms, last_live);
  const bool first = !m_applied.exchange(true, std::memory_order_acq_rel);
  if (!first && !changed) {
    return;
  }
  m_slate.store(m_decider.slate(), std::memory_order_release);
  m_switches.store(m_decider.switches(), std::memory_order_relaxed);
  if (changed && m_log) {
    m_log(std::format("standby: {} ({} ms since the last live payload)\n",
                      m_decider.slate() ? "slate" : "live",
                      m_last_live_ms_ago.load(std::memory_order_relaxed)));
  }
  if (m_apply) {
    m_apply(m_decider.slate());
  }
}

auto standby_controller::mode_name() const -> const char*
{
  return slate() ? "slate" : "live";
}

}  // namespace standby
