// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_RING_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_RING_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// ts_ring — the single new core component of the transport profile
// (TRANSPORT_PROFILE.md §3.4): a single-producer / multi-consumer byte ring
// holding the incoming MPEG-TS byte stream as received (7×188-aligned chunks).
//
// Invariants (contract-level, do not weaken):
//  - The PRODUCER NEVER BLOCKS. The RIST callback memcpys into the ring,
//    advances the write position, wakes consumers, returns. Slow consumers
//    cannot back-pressure ingest — stalling the RIST worker thread jeopardises
//    OOB telemetry and RIST housekeeping.
//  - Slow-consumer policy is DROP-OLDEST, PER CONSUMER: a consumer that falls
//    a full ring behind has its cursor advanced to the oldest valid position
//    at a TS-packet (188-byte) boundary and its dropped_bytes counter
//    incremented. Other consumers are unaffected.
//  - Consumers each own an independent cursor; positions are absolute byte
//    offsets since ring construction (monotonic uint64), mapped into the
//    buffer by power-of-two mask.
//
// Reader validation: because the producer overwrites without waiting, a
// consumer's copy may be lapped mid-read. read() therefore re-validates after
// copying (seqlock style): if the producer advanced far enough to have
// overwritten any part of the copied region, the copy is discarded, the cursor
// jumps forward (drop-oldest) and the read retries. The producer stays
// oblivious to consumers by design.
class ts_ring
{
public:
  static constexpr std::size_t k_min_bytes = 16UL * 1024 * 1024;   // 16 MiB
  static constexpr std::size_t k_max_bytes = 256UL * 1024 * 1024;  // 256 MiB
  static constexpr std::size_t k_ts_packet = 188;
  static constexpr int k_max_consumers = 16;  // 8 outputs + recorder + slack

  // Ring sizing rule (H6 / FIXPLAN M1.3): 10 seconds at the session's
  // ingest.bandwidth (kbps), clamped to [16 MiB, 256 MiB], rounded up to a
  // power of two (the max is itself a power of two, so the clamp holds).
  static auto size_for_bandwidth(int bandwidth_kbps) noexcept -> std::size_t;

  // capacity_bytes is rounded up to a power of two and clamped to
  // [k_min_bytes, k_max_bytes].
  explicit ts_ring(std::size_t capacity_bytes);

  ts_ring(const ts_ring&) = delete;
  auto operator=(const ts_ring&) -> ts_ring& = delete;
  ts_ring(ts_ring&&) = delete;
  auto operator=(ts_ring&&) -> ts_ring& = delete;
  ~ts_ring() = default;

  [[nodiscard]] auto capacity() const noexcept -> std::size_t
  {
    return m_capacity;
  }

  // ---- producer side (exactly one thread; the RIST data callback) ----

  // Copy len bytes into the ring and wake consumers. Never blocks (the only
  // lock taken is a short, uncontended wakeup mutex; no wait on any consumer).
  // len == 0 is a no-op. len > capacity is truncated to the LAST capacity
  // bytes (cannot happen with 1316-byte RIST payloads; defensive only).
  auto write(const uint8_t* data, std::size_t len) -> void;

  // Total bytes ever written (absolute head position).
  [[nodiscard]] auto head() const noexcept -> uint64_t
  {
    return m_write_pos.load(std::memory_order_acquire);
  }

  // ---- consumer side ----

  // Register a consumer whose cursor starts at the CURRENT head (a new output
  // joins live; it never sees backlog it wasn't attached for). Returns a
  // consumer id, or -1 if all slots are taken. Thread-safe.
  auto add_consumer() -> int;

  // Release a consumer slot. Concurrent read() on the same id is the caller's
  // bug (an output destroys its feeder thread before detaching).
  auto remove_consumer(int con) -> void;

  // Copy up to max_len bytes for consumer `con` into dst. Blocks until data is
  // available, the timeout elapses, or the ring is closed. Returns the number
  // of bytes copied (0 on timeout or close). Applies drop-oldest + accounting
  // when the consumer has been lapped.
  auto read(int con,
            uint8_t* dst,
            std::size_t max_len,
            std::chrono::milliseconds timeout) -> std::size_t;

  // Ring-drop accounting for /stats (TRANSPORT_PROFILE §1.4).
  [[nodiscard]] auto dropped_bytes(int con) const noexcept -> uint64_t;

  // Wake every blocked consumer permanently (session teardown). Idempotent.
  auto close() -> void;

  [[nodiscard]] auto closed() const noexcept -> bool
  {
    return m_closed.load(std::memory_order_acquire);
  }

private:
  struct consumer_slot
  {
    std::atomic<uint64_t> cursor {0};
    std::atomic<uint64_t> dropped {0};
    std::atomic_bool active {false};
  };

  // Round the cursor of a lapped consumer forward to the oldest valid
  // position, aligned UP to a TS-packet boundary, and account the loss.
  auto apply_drop_oldest(consumer_slot& slot,
                         uint64_t cursor,
                         uint64_t head_now) const -> uint64_t;

  std::size_t m_capacity;  // power of two
  std::size_t m_mask;
  std::vector<uint8_t> m_buf;

  std::atomic<uint64_t> m_write_pos {0};
  std::atomic_bool m_closed {false};

  // Wakeup only — never protects the data path. The producer takes it for an
  // empty critical section before notify_all so a consumer that checked the
  // head and is about to sleep cannot miss the wakeup.
  mutable std::mutex m_wake_mutex;
  std::condition_variable m_wake_cv;

  mutable std::mutex m_slots_mutex;  // add/remove_consumer only
  std::array<consumer_slot, k_max_consumers> m_slots;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_RING_H
