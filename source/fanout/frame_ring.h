// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_FRAME_RING_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_FRAME_RING_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// frame_ring — the SPMC hand-off for the opt-in transcode tier (schema_version
// 3). The later shared decode stage decodes the ingest elementary stream ONCE
// and publishes raw frames here; each transcode output pipeline attaches an
// independent cursor and pulls whole frames to re-encode.
//
// Deliberately GStreamer-free: the ring stores a POD frame descriptor plus the
// frame bytes in a fixed-size byte arena — never a GstBuffer*. That keeps the
// unit test runnable in the tests-only build lane (OBR_TESTS_ONLY=ON, no
// GStreamer/librist); any GstBuffer glue belongs in the decoder/output.
//
// Invariants (contract-level, do not weaken):
//  - The PRODUCER NEVER BLOCKS. The decoder thread copies the frame bytes,
//    publishes the descriptor, wakes consumers and returns. Slow consumers
//    cannot back-pressure decode.
//  - Slow-consumer policy is DROP-OLDEST, PER CONSUMER **AT WHOLE-FRAME
//    BOUNDARIES** (unlike ts_ring, which drops at 188-byte TS-packet
//    boundaries): a consumer that falls a whole ring behind has its cursor
//    advanced to the oldest frame still fully present and its dropped_frames
//    counter incremented by the number of frames skipped. A frame is delivered
//    whole or not at all — never split.
//  - Consumers each own an independent cursor measured in absolute frames since
//    ring construction (monotonic uint64). Descriptors live in a power-of-two
//    slot array indexed by cursor & mask; a slot is only overwritten by the
//    frame k_max_frames ahead of it.
//
// Reader validation: because the producer overwrites without waiting, a
// consumer's copy may be lapped mid-read. read() re-validates after copying
// (seqlock style): if the producer advanced far enough either to have recycled
// the descriptor slot (frame distance > k_max_frames) or to have overwritten
// any byte of the copied frame, the copy is discarded, the cursor jumps forward
// (drop-oldest) and the read retries. The producer stays oblivious by design.
class frame_ring
{
public:
  static constexpr std::size_t k_min_bytes = 16UL * 1024 * 1024;   // 16 MiB
  static constexpr std::size_t k_max_bytes = 256UL * 1024 * 1024;  // 256 MiB
  static constexpr std::size_t k_max_frames = 512;                 // pow2 slots
  static constexpr int k_max_consumers = 16;  // 8 outputs + recorder + slack

  // flags bit 0: the frame is a keyframe (IDR / sync point).
  static constexpr std::uint32_t k_flag_keyframe = 1U;

  // POD frame descriptor. `format`/`stride` are opaque integers owned by the
  // producer's pipeline (e.g. a GstVideoFormat value) — the ring never
  // interprets them. `size` is filled by write() from the payload length.
  struct frame_meta
  {
    std::uint64_t pts = 0;      // presentation timestamp (producer timebase)
    std::uint64_t dts = 0;      // decode timestamp (producer timebase)
    std::uint32_t flags = 0;    // see k_flag_keyframe
    std::int32_t format = 0;    // opaque pixel-format id
    std::int32_t stride = 0;    // bytes per row; 0 if planar/unknown
    std::uint32_t size = 0;     // payload bytes (set by write)
    auto operator==(const frame_meta&) const -> bool = default;
  };

  // capacity_bytes is rounded up to a power of two and clamped to
  // [k_min_bytes, k_max_bytes].
  explicit frame_ring(std::size_t capacity_bytes);

  frame_ring(const frame_ring&) = delete;
  auto operator=(const frame_ring&) -> frame_ring& = delete;
  frame_ring(frame_ring&&) = delete;
  auto operator=(frame_ring&&) -> frame_ring& = delete;
  ~frame_ring() = default;

  [[nodiscard]] auto capacity() const noexcept -> std::size_t
  {
    return m_capacity;
  }

  [[nodiscard]] auto max_frames() const noexcept -> std::size_t
  {
    return k_max_frames;
  }

  // ---- producer side (exactly one thread; the shared decoder) ----

  // Copy a whole frame — `len` payload bytes plus `meta` — into the ring and
  // wake consumers. Never blocks (the only lock taken is a short, uncontended
  // wakeup mutex; no wait on any consumer). Returns false (the frame is
  // dropped whole, the ring is untouched) when len == 0 or len > capacity: a
  // frame is never truncated or split, because a partial raw frame is garbage
  // to any decoder/encoder downstream. meta.size is overwritten from len.
  auto write(const frame_meta& meta, const std::uint8_t* data, std::size_t len)
      -> bool;

  // Total frames ever written (absolute head position).
  [[nodiscard]] auto head() const noexcept -> std::uint64_t
  {
    return m_head_frame.load(std::memory_order_acquire);
  }

  // ---- consumer side ----

  // Register a consumer whose cursor starts at the CURRENT head (a new output
  // joins live; it never sees frames it wasn't attached for). Returns a
  // consumer id, or -1 if all slots are taken. Thread-safe.
  auto add_consumer() -> int;

  // Release a consumer slot. Concurrent read() on the same id is the caller's
  // bug (an output destroys its feeder thread before detaching).
  auto remove_consumer(int con) -> void;

  // Read the next WHOLE frame for consumer `con`: on success copies its payload
  // into `dst` (which must hold at least capacity() bytes — the largest frame
  // the ring can store) and writes its metadata (with the true size) to `out`.
  // Blocks until a frame is available, the timeout elapses, or the ring is
  // closed. Returns the frame size copied, or 0 on timeout/close/detach (or
  // when dst is too small for the pending frame). Applies whole-frame
  // drop-oldest + accounting when the consumer has been lapped.
  auto read(int con,
            frame_meta& out,
            std::uint8_t* dst,
            std::size_t max_len,
            std::chrono::milliseconds timeout) -> std::size_t;

  // Whole-frame drop accounting for /stats.
  [[nodiscard]] auto dropped_frames(int con) const noexcept -> std::uint64_t;

  // Wake every blocked consumer permanently (session teardown). Idempotent.
  auto close() -> void;

  [[nodiscard]] auto closed() const noexcept -> bool
  {
    return m_closed.load(std::memory_order_acquire);
  }

private:
  struct frame_slot
  {
    frame_meta meta;
    std::uint64_t arena_offset = 0;  // absolute arena position of the payload
  };

  struct consumer_slot
  {
    std::atomic<std::uint64_t> cursor {0};
    std::atomic<std::uint64_t> dropped {0};
    std::atomic_bool active {false};
  };

  // Round the cursor of a lapped consumer forward to the oldest frame still
  // fully present (skipping any whose payload the producer has begun
  // overwriting), and account the loss in whole frames.
  auto apply_drop_oldest(consumer_slot& slot,
                         std::uint64_t cursor,
                         std::uint64_t head_now,
                         std::uint64_t arena_now) const -> std::uint64_t;

  std::size_t m_capacity;  // power of two
  std::size_t m_mask;
  std::vector<std::uint8_t> m_buf;

  std::array<frame_slot, k_max_frames> m_frames;
  static constexpr std::size_t m_frame_mask = k_max_frames - 1;

  std::atomic<std::uint64_t> m_arena_pos {0};  // bytes ever written to arena
  std::atomic<std::uint64_t> m_head_frame {0};
  std::atomic_bool m_closed {false};

  // Wakeup only — never protects the data path. The producer takes it for an
  // empty critical section before notify_all so a consumer that checked the
  // head and is about to sleep cannot miss the wakeup.
  mutable std::mutex m_wake_mutex;
  std::condition_variable m_wake_cv;

  mutable std::mutex m_slots_mutex;  // add/remove_consumer only
  std::array<consumer_slot, k_max_consumers> m_slots;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_FRAME_RING_H
