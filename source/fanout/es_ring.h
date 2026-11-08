// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_ES_RING_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_ES_RING_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// es_ring — the single-producer / multi-consumer ring that carries COMPRESSED
// elementary-stream access units (video, and the same class again for audio)
// out of the shared standby stage and into every fan-out consumer.
//
// It exists because ts_ring cannot be used here: ts_ring's drop granularity is
// a 188-byte TS packet, which is meaningless once a demuxer has been passed. An
// access unit is the smallest thing a decoder or a muxer can be handed; a
// partial AU is not degraded media, it is corruption (a decoder fed half an IDR
// emits garbage or errors out, and a copy destination muxes a broken frame).
// The drop granularity here is therefore a WHOLE access unit, exactly as in
// frame_ring.
//
// Invariants (contract-level, do not weaken):
//  - The PRODUCER NEVER BLOCKS. write() memcpys into the arena, publishes the
//    descriptor, advances the head and wakes consumers. A slow consumer cannot
//    back-pressure the stage: stalling the pad probe that feeds this ring would
//    stall the demux feeding every other output.
//  - Slow-consumer policy is DROP-OLDEST, PER CONSUMER, AT AU BOUNDARIES: a
//    consumer that falls out of the arena has its cursor advanced to the oldest
//    AU that is still fully present, and dropped_aus() counts every AU it never
//    saw. Other consumers are unaffected.
//  - A consumer CANNOT be handed a partial AU. Cursors are AU indices (not byte
//    offsets), so advancing a cursor is always a whole-AU operation, and the
//    copy is re-validated after the fact (seqlock style, see below).
//
// Storage layout (the invariant that makes the above true):
//  - An ARENA of capacity() bytes, treated as a circular byte buffer. Each AU
//    occupies a contiguous, never-interleaved range
//    [arena_offset, arena_offset + size) with size <= capacity(), so an AU is
//    fully present iff (arena_now - arena_offset) <= capacity().
//  - A DESCRIPTOR ARRAY of k_max_aus au_meta slots (k_max_aus is a power of
//    two), indexed by AU index & (k_max_aus - 1). A cursor more than k_max_aus
//    behind the head points at a recycled slot; that is the second lapping
//    condition and it is clamped before the byte scan.
//  - So the ring holds at least min(capacity_bytes worth of AUs, k_max_aus AUs).
//    Sizing (size_for_bps) keeps the arena at ten seconds of the session's
//    compressed bitrate, which is far more than a decoder needs to catch up.
//
// Reader validation: the producer overwrites without waiting, so a consumer's
// copy may be lapped mid-read. read() re-validates after copying: if the
// producer has since overwritten any byte of the copied AU or recycled its
// descriptor, the copy is discarded, the cursor jumps forward (drop-oldest) and
// the read retries. The producer stays oblivious to consumers by design.
class es_ring
{
public:
  static constexpr std::size_t k_min_bytes = 4UL * 1024 * 1024;   // 4 MiB
  static constexpr std::size_t k_max_bytes = 64UL * 1024 * 1024;  // 64 MiB
  static constexpr std::size_t k_max_aus = 512;  // power of two, ~17 s at 30 fps
  static constexpr int k_max_consumers = 16;     // 8 outputs + recorder + slack

  // One access unit's descriptor. `seq` is assigned BY THE RING on write (the
  // AU's absolute index since construction) — the ring is the authority on
  // ordering, and a consumer can spot a gap by comparing consecutive seqs.
  // A caller-supplied seq is ignored.
  struct au_meta
  {
    std::int64_t pts_ns = 0;
    std::int64_t dts_ns = 0;
    std::uint64_t seq = 0;
    bool keyframe = false;
    std::size_t size = 0;  // bytes; set by write(), never by the caller
  };

  // Ring sizing rule: ten seconds at the session's compressed bitrate (kbps),
  // clamped to [k_min_bytes, k_max_bytes] and rounded up to a power of two (the
  // ceiling is itself a power of two, so the clamp holds). kbps <= 0 falls back
  // to the floor.
  static auto size_for_bps(int kbps) noexcept -> std::size_t;

  // capacity_bytes is rounded up to a power of two and clamped to
  // [k_min_bytes, k_max_bytes].
  explicit es_ring(std::size_t capacity_bytes);

  es_ring(const es_ring&) = delete;
  auto operator=(const es_ring&) -> es_ring& = delete;
  es_ring(es_ring&&) = delete;
  auto operator=(es_ring&&) -> es_ring& = delete;
  ~es_ring() = default;

  [[nodiscard]] auto capacity() const noexcept -> std::size_t
  {
    return m_capacity;
  }

  // ---- producer side (exactly one thread: the stage's pad probe) ----

  // Copy one whole AU into the ring and wake consumers. Never blocks (the only
  // lock taken is a short, uncontended wakeup mutex; no wait on any consumer).
  // Returns false and does nothing when data is null, len is 0, or len exceeds
  // the arena — an AU that cannot be stored whole is not stored at all.
  auto write(const au_meta& meta, const std::uint8_t* data, std::size_t len)
      -> bool;

  // Absolute number of AUs ever written (the head index).
  [[nodiscard]] auto head() const noexcept -> std::uint64_t
  {
    return m_head_au.load(std::memory_order_acquire);
  }

  // ---- consumer side ----

  // Register a consumer whose cursor starts at the CURRENT head (a new output
  // joins live; it never sees backlog it was not attached for). Returns a
  // consumer id, or -1 if all slots are taken. Thread-safe.
  auto add_consumer() -> int;

  // Release a consumer slot. Concurrent read() on the same id is the caller's
  // bug (an output destroys its feeder thread before detaching).
  auto remove_consumer(int con) -> void;

  // Copy one whole AU for consumer `con` into dst (cap bytes). Blocks until an
  // AU is available, the timeout elapses, or the ring is closed. Returns the
  // number of bytes copied (0 on timeout or close). Applies drop-oldest +
  // accounting when the consumer has been lapped; an AU larger than `cap` is
  // skipped whole (counted as a drop) rather than truncated.
  auto read(int con,
            au_meta& out,
            std::uint8_t* dst,
            std::size_t cap,
            std::chrono::milliseconds timeout) -> std::size_t;

  // AU-drop accounting for /stats: how many whole AUs this consumer never saw.
  [[nodiscard]] auto dropped_aus(int con) const noexcept -> std::uint64_t;

  // Wake every blocked consumer permanently (session teardown). Idempotent.
  auto close() -> void;

  [[nodiscard]] auto closed() const noexcept -> bool
  {
    return m_closed.load(std::memory_order_acquire);
  }

private:
  struct consumer_slot
  {
    std::atomic<std::uint64_t> cursor {0};  // absolute AU index
    std::atomic<std::uint64_t> dropped {0};
    std::atomic_bool active {false};
  };

  struct au_slot
  {
    au_meta meta {};
    std::uint64_t arena_offset = 0;  // absolute byte offset of the payload
  };

  // Round the cursor of a lapped consumer forward to the oldest AU that is
  // still fully present, and account the AUs skipped.
  auto apply_drop_oldest(consumer_slot& slot,
                         std::uint64_t cursor,
                         std::uint64_t head_now,
                         std::uint64_t arena_now) const -> std::uint64_t;

  std::size_t m_capacity;  // power of two
  std::size_t m_mask;
  std::size_t m_slot_mask;  // k_max_aus - 1
  std::vector<std::uint8_t> m_buf;
  std::vector<au_slot> m_aus;  // k_max_aus descriptors

  std::atomic<std::uint64_t> m_arena_pos {0};  // absolute bytes written
  std::atomic<std::uint64_t> m_head_au {0};    // absolute AUs published
  std::atomic_bool m_closed {false};

  // Wakeup only — never protects the data path. The producer takes it for an
  // empty critical section before notify_all so a consumer that checked the
  // head and is about to sleep cannot miss the wakeup.
  mutable std::mutex m_wake_mutex;
  std::condition_variable m_wake_cv;

  mutable std::mutex m_slots_mutex;  // add/remove_consumer only
  std::array<consumer_slot, k_max_consumers> m_slots;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_ES_RING_H
