// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_CONTROLLER_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_CONTROLLER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

// controller — WHEN the session is on the slate, kept separate from HOW the
// stage switches to it.
//
// The stage (stage.h) owns the pipeline and the keyframe-accurate switch. This
// file owns the policy: how much silence means "the live feed is gone" and how
// much returning media means "the live feed is really back". Splitting the two
// is what makes the policy unit-testable without a media stack, and it keeps
// the stage free of any clock of its own.
//
// Everything here is pure C++ (no GStreamer, no pipeline lock): the stage is
// reached only through the apply callback, which the receiver wires to
// stage::set_slate(). A controller thread samples the same monotonic
// last-live timestamp the idle watchdog already reads, so the two can never
// disagree about whether media is arriving.
namespace standby
{

// The controller's sampling period. Every window below is asserted against it:
// it is far below the smallest live_gap_ms the wire accepts (100 ms), so a
// switch lands within one tick of its window.
inline constexpr std::int64_t k_sample_ms = 50;

// Why a session is allowed to end on its own (§1.5). Two policies, never both:
//  - standby OFF is today's rule, unchanged: --idle-timeout seconds with no
//    RIST payload ends the session (0 = the watchdog is off).
//  - standby ON has a slate on air, so "no payload" is normal operation. The
//    session only dies after max_live_gap_s with no LIVE media at all: a
//    destination can be held indefinitely on the slate, but an ingest that
//    never returns is a dead session (0 = no limit).
struct session_end_policy
{
  bool standby_enabled = false;
  int idle_timeout_s = 0;     // --idle-timeout (standby OFF); 0 = off
  int max_live_gap_s = 1800;  // standby ON; 0 = no limit
};

// True when the session should be stopped. `last_live_ms` is the steady-clock
// time of the last live payload, or 0 when there has never been one — in which
// case the answer is always false: "no payload yet" is the agent's pre-start
// TTL, not this watchdog's, exactly as before.
auto should_end_session(std::int64_t now_ms,
                        std::int64_t last_live_ms,
                        const session_end_policy& policy) -> bool;

// The hysteresis machine, with no clock and no threads: feed it observations,
// it answers "slate or live".
//
// live -> slate as soon as the payload has been absent for live_gap_ms.
// slate -> live only after live_hold_ms of CONTINUOUS payload, where continuity
// is judged by the same live_gap_ms window: a feed that flaps faster than the
// hold window never gets switched back to, so a marginal link does not put the
// slate on and off air once a second.
class mode_decider
{
public:
  // `start_on_slate` mirrors where the stage actually starts: the stage begins
  // on the slate when it has one (stage::start calls set_slate(ready)), and the
  // decider must not disagree with it on its first tick or it would switch the
  // slate straight back off.
  mode_decider(int live_gap_ms,
               int live_hold_ms,
               bool start_on_slate = false);

  // Feed one observation. Returns true when the desired source changed.
  auto feed(std::int64_t now_ms, std::int64_t last_live_ms) -> bool;

  [[nodiscard]] auto slate() const noexcept -> bool { return m_slate; }
  [[nodiscard]] auto switches() const noexcept -> std::uint64_t
  {
    return m_switches;
  }

private:
  // 1, not 0: a zero gap window would mean "every observation is a gap" and
  // the decider could never return to live. The wire refuses 0 anyway.
  [[nodiscard]] auto gap_ms() const noexcept -> std::int64_t
  {
    return m_live_gap_ms > 0 ? m_live_gap_ms : 1;
  }
  [[nodiscard]] auto hold_ms() const noexcept -> std::int64_t
  {
    return m_live_hold_ms > 0 ? m_live_hold_ms : 0;
  }

  int m_live_gap_ms = 1500;
  int m_live_hold_ms = 500;
  bool m_slate = false;
  // Steady-clock ms of the first observation: with no live payload ever seen,
  // the silence is measured from here, so a standby session that is started
  // before its encoder puts the slate up instead of holding a black live path.
  // A separate flag, not 0, because 0 is a legitimate clock reading.
  bool m_have_epoch = false;
  std::int64_t m_epoch_ms = 0;
  // While on the slate: when the returning payload started to hold.
  bool m_live_run = false;
  std::int64_t m_live_since_ms = 0;
  std::uint64_t m_switches = 0;
};

// standby_controller — the sampling thread and the observable state.
//
// It reads `last_live_ms` (the atomic the RIST path already bumps) rather than
// being called from the receive path: one lock-free load every tick cannot
// perturb packet handling, and it means an ingest that stops inside librist
// cannot stall the decision that puts the slate on air.
class standby_controller
{
public:
  using apply_fn = std::function<void(bool slate)>;
  using log_fn = std::function<void(const std::string&)>;
  using clock_fn = std::function<std::int64_t()>;

  // How often the thread samples. Well below the smallest live_gap_ms the wire
  // accepts (100 ms), so a switch lands within one tick of the window.
  static constexpr std::int64_t k_tick_ms = k_sample_ms;

  // `slate_available` false means the session has no rendered slate (a failed
  // render): the controller then stays live and never asks the stage for a
  // slate it cannot show, so /status cannot report a switch that never happens.
  standby_controller(const std::atomic<std::int64_t>& last_live_ms,
                     int live_gap_ms,
                     int live_hold_ms,
                     apply_fn apply,
                     bool slate_available = true,
                     log_fn log = {},
                     clock_fn clock = {});
  ~standby_controller();

  standby_controller(const standby_controller&) = delete;
  auto operator=(const standby_controller&) -> standby_controller& = delete;
  standby_controller(standby_controller&&) = delete;
  auto operator=(standby_controller&&) -> standby_controller& = delete;

  // Sample immediately, then every k_tick_ms until stop(). Idempotent.
  auto start() -> void;
  auto stop() -> void;

  // One observation, applied if the decision changed. Public so a test can
  // drive the machine deterministically with its own clock.
  auto tick(std::int64_t now_ms) -> void;

  // ---- observability (atomics only; /status reads these) ----
  [[nodiscard]] auto slate() const noexcept -> bool
  {
    return m_slate.load(std::memory_order_acquire);
  }
  // "live" | "slate", the source the controller is asking for.
  [[nodiscard]] auto mode_name() const -> const char*;
  [[nodiscard]] auto switches() const noexcept -> std::uint64_t
  {
    return m_switches.load(std::memory_order_relaxed);
  }
  // Age of the last live payload, ms; 0 when none has arrived yet.
  [[nodiscard]] auto last_live_ms_ago() const noexcept -> std::int64_t
  {
    return m_last_live_ms_ago.load(std::memory_order_relaxed);
  }

private:
  auto run() -> void;

  const std::atomic<std::int64_t>& m_last_live_ms;
  mode_decider m_decider;
  apply_fn m_apply;
  bool m_slate_available = true;
  log_fn m_log;
  clock_fn m_clock;

  std::thread m_thread;
  std::atomic<bool> m_stopping {false};
  std::atomic<bool> m_running {false};
  std::atomic<bool> m_slate {false};
  std::atomic<bool> m_applied {false};
  std::atomic<std::uint64_t> m_switches {0};
  std::atomic<std::int64_t> m_last_live_ms_ago {0};
};

}  // namespace standby

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_CONTROLLER_H
