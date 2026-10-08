// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_RECEIVE_RECEIVE_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_RECEIVE_RECEIVE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "RISTNet.h"

#include "lib/lib.h"

// rist_receive wraps a RISTNetReceiver that LISTENS for the encoder's
// RISTNetSender (caller). Received MPEG-TS payloads are handed to a push
// callback — under the transport profile that callback writes the SPMC ring
// and MUST return immediately (the producer never blocks; TRANSPORT_PROFILE
// §3.4). ~1 Hz it computes link quality + worst-case RTT and sends the 5-byte
// wan_telemetry struct back over the RIST OOB channel (closing the encoder's
// remote_oob adaptive-bitrate loop), and snapshots the full receiver_flow
// stats for the agent-facing GET /stats.
//
// Bonding: multiple RIST peers arrive on this ONE listener port (one peer per
// WAN path via rist2rist). Every peer feeds the same flow; peer connects and
// disconnects mid-session are normal (cellular CGNAT churn) and never restart
// anything. OOB telemetry goes to the most recent data peer (the relay
// forwards it upstream).
class rist_receive
{
public:
  using push_fn = std::function<int(const uint8_t*, std::size_t)>;

  explicit rist_receive(std::function<void(const std::string&)> log);
  ~rist_receive();
  rist_receive(const rist_receive&) = delete;
  auto operator=(const rist_receive&) -> rist_receive& = delete;
  rist_receive(rist_receive&&) = delete;
  auto operator=(rist_receive&&) -> rist_receive& = delete;

  // Build the listener, wire callbacks (ADVANCED profile) and start receiving.
  // `state` is borrowed for telemetry mirroring + media-liveness counters.
  // opts.psk (with opts.psk_aes) enables librist PSK on the listener — the
  // secret travels via RISTNetReceiverSettings, never inside the URL.
  auto start(const receiver_config& cfg,
             const runtime_options& opts,
             receiver_state* state,
             push_fn push,
             std::string& err) -> bool;

  auto stop() -> void;

  // Copy of the latest ~1 Hz receiver_flow snapshot (for GET /stats).
  [[nodiscard]] auto flow_stats() const -> rist_flow_stat;

private:
  auto log(const std::string& msg) const -> void;

  std::function<void(const std::string&)> m_log_func;
  std::unique_ptr<RISTNetReceiver> m_receiver;
  push_fn m_push;
  receiver_state* m_state = nullptr;  // borrowed

  // The most recent data peer, for OOB telemetry. Accessed from librist
  // callback threads.
  std::atomic<rist_peer*> m_peer {nullptr};
  std::atomic_bool m_started {false};

  mutable std::mutex m_flow_mutex;
  rist_flow_stat m_flow;

  // ~1 s window for the OOB telemetry and for the GET /stats quality figure.
  //
  // librist delivers receiver_flow counters as PER-INTERVAL deltas —
  // rist_receiver_flow_statistics() memsets flow->stats_instant on every
  // emission — and it emits far more often than the ~1 Hz the wrapper asks for
  // (the receiver flow's stats interval is never applied upstream, so the
  // loop's `stats_next_time += stats_report_time` gate never advances and the
  // callback runs at the protocol loop's event rate: measured on the rig at
  // ~15/s idle and ~150/s under loss). A quality ratio over a ~7 ms window
  // swings across its whole range on a single NACK, which is what ratcheted the
  // encoder's ABR to the floor. Summing the deltas here reconstructs the true
  // ~1 s window, so the OOB control signal matches the shape of the encoder's
  // local (sender-side) figure.
  static constexpr int64_t k_oob_interval_ms = 1000;
  mutable std::mutex m_acc_mutex;
  int64_t m_acc_start_ms = 0;
  uint64_t m_acc_received = 0;
  uint64_t m_acc_missing = 0;
  uint64_t m_acc_recovered = 0;
  uint64_t m_acc_recovered_one = 0;
  uint64_t m_acc_lost = 0;
  uint64_t m_acc_reordered = 0;
  uint64_t m_acc_samples = 0;  // per-window emission count (diagnostic)
  uint32_t m_acc_worst_rtt = 0;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_RECEIVE_RECEIVE_H
