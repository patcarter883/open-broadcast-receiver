// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/es_ring.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace
{
// Ten seconds of the session's compressed bitrate, the same hold time ts_ring
// uses for the transport stream: long enough that a decoder that stalls briefly
// catches up without drops, short enough that a stalled output does not pin a
// session's worth of memory.
constexpr std::int64_t k_hold_seconds = 10;
constexpr std::int64_t k_bits_per_kbit = 1000;
constexpr std::int64_t k_bits_per_byte = 8;

auto round_up_pow2(std::size_t bytes) -> std::size_t
{
  return std::bit_ceil(
      std::clamp(bytes, es_ring::k_min_bytes, es_ring::k_max_bytes));
}
}  // namespace

auto es_ring::size_for_bps(int kbps) noexcept -> std::size_t
{
  if (kbps <= 0) {
    return k_min_bytes;
  }
  const std::int64_t bytes = static_cast<std::int64_t>(kbps)
                             * k_bits_per_kbit / k_bits_per_byte
                             * k_hold_seconds;
  // The clamp inside round_up_pow2 also keeps the conversion safe for absurd
  // bitrates (the product stays inside int64 for any int kbps).
  const std::size_t clamped = static_cast<std::size_t>(
      std::clamp<std::int64_t>(bytes, static_cast<std::int64_t>(k_min_bytes),
                               static_cast<std::int64_t>(k_max_bytes)));
  return round_up_pow2(clamped);
}

es_ring::es_ring(std::size_t capacity_bytes)
    : m_capacity {round_up_pow2(capacity_bytes)}
    , m_mask {m_capacity - 1}
    , m_slot_mask {k_max_aus - 1}
    , m_buf(m_capacity)
    , m_aus(k_max_aus)
{
  static_assert((k_max_aus & (k_max_aus - 1)) == 0,
                "descriptor count must be a power of two to mask the index");
}

auto es_ring::write(const au_meta& meta,
                    const std::uint8_t* data,
                    std::size_t len) -> bool
{
  // An access unit is delivered whole or not at all: an empty or oversized
  // payload is refused without touching the ring. Half an AU is corruption to
  // every decoder and muxer downstream, so there is no safe partial write. The
  // producer never blocks.
  if (data == nullptr || len == 0 || len > m_capacity) {
    return false;
  }

  const std::uint64_t arena_pos = m_arena_pos.load(std::memory_order_relaxed);
  const std::uint64_t au_idx = m_head_au.load(std::memory_order_relaxed);
  const std::size_t off = static_cast<std::size_t>(arena_pos) & m_mask;
  const std::size_t first = std::min(len, m_capacity - off);
  std::memcpy(&m_buf[off], data, first);
  if (first < len) {
    std::memcpy(m_buf.data(), data + first, len - first);
  }

  au_slot& slot = m_aus[static_cast<std::size_t>(au_idx) & m_slot_mask];
  slot.meta = meta;
  slot.meta.size = len;  // the ring owns the stored size
  slot.meta.seq = au_idx;
  slot.arena_offset = arena_pos;

  // Publish the payload position first, then the new head: the head release
  // store is the single publication point a consumer acquire-loads, and it
  // orders the descriptor store and the byte copies before it.
  m_arena_pos.store(arena_pos + len, std::memory_order_release);
  m_head_au.store(au_idx + 1, std::memory_order_release);

  // Empty critical section pairs with the consumer's predicate check under the
  // same mutex: either the consumer saw the new head before sleeping, or it is
  // asleep and this notify wakes it. The producer never waits on consumers.
  {
    std::lock_guard<std::mutex> guard(m_wake_mutex);
  }
  m_wake_cv.notify_all();
  return true;
}

auto es_ring::add_consumer() -> int
{
  std::lock_guard<std::mutex> guard(m_slots_mutex);
  for (int idx = 0; idx < k_max_consumers; ++idx) {
    consumer_slot& slot = m_slots[static_cast<std::size_t>(idx)];
    if (!slot.active.load(std::memory_order_acquire)) {
      slot.cursor.store(m_head_au.load(std::memory_order_acquire),
                        std::memory_order_release);
      slot.dropped.store(0, std::memory_order_release);
      slot.active.store(true, std::memory_order_release);
      return idx;
    }
  }
  return -1;
}

auto es_ring::remove_consumer(int con) -> void
{
  if (con < 0 || con >= k_max_consumers) {
    return;
  }
  std::lock_guard<std::mutex> guard(m_slots_mutex);
  m_slots[static_cast<std::size_t>(con)].active.store(false,
                                                      std::memory_order_release);
}

auto es_ring::apply_drop_oldest(consumer_slot& slot,
                                std::uint64_t cursor,
                                std::uint64_t head_now,
                                std::uint64_t arena_now) const -> std::uint64_t
{
  std::uint64_t dropped = 0;

  // Descriptor lapping: a cursor more than k_max_aus behind the head points at
  // a slot a newer AU has already reused. Jump to the oldest AU whose
  // descriptor is still intact (this also bounds the byte scan below). The jump
  // is in whole AUs, so the cursor still lands on an AU boundary.
  if (head_now - cursor > k_max_aus) {
    const std::uint64_t jump = head_now - k_max_aus - cursor;
    cursor += jump;
    dropped += jump;
  }

  // Arena lapping: the arena retains only the last capacity() bytes, so skip
  // every AU whose payload the producer has already begun to overwrite. Each
  // skip is one whole access unit. After the descriptor clamp there are at most
  // k_max_aus AUs to inspect.
  while (cursor < head_now
         && (arena_now
             - m_aus[static_cast<std::size_t>(cursor) & m_slot_mask].arena_offset)
             > m_capacity)
  {
    ++cursor;
    ++dropped;
  }

  if (dropped != 0) {
    slot.dropped.fetch_add(dropped, std::memory_order_relaxed);
  }
  slot.cursor.store(cursor, std::memory_order_release);
  return cursor;
}

auto es_ring::read(int con,
                   au_meta& out,
                   std::uint8_t* dst,
                   std::size_t cap,
                   std::chrono::milliseconds timeout) -> std::size_t
{
  if (con < 0 || con >= k_max_consumers || dst == nullptr || cap == 0) {
    return 0;
  }
  consumer_slot& slot = m_slots[static_cast<std::size_t>(con)];
  if (!slot.active.load(std::memory_order_acquire)) {
    return 0;
  }

  std::uint64_t cursor = slot.cursor.load(std::memory_order_acquire);

  // Wait for an AU (or closure). The predicate reads the head under the wakeup
  // mutex, pairing with the producer's empty critical section.
  {
    std::unique_lock<std::mutex> wait_lock(m_wake_mutex);
    const bool ready = m_wake_cv.wait_for(
        wait_lock,
        timeout,
        [&]() -> bool
        {
          return m_closed.load(std::memory_order_acquire)
              || m_head_au.load(std::memory_order_acquire) > cursor;
        });
    if (!ready) {
      return 0;  // timeout
    }
  }

  // Seqlock-style copy: validate afterwards that the producer did not recycle
  // the descriptor slot or overwrite the AU; retry with a jumped cursor if it
  // did. A cursor is an AU index throughout, so every jump is a whole-AU jump
  // and no path can deliver a fragment.
  for (;;) {
    const std::uint64_t head_now = m_head_au.load(std::memory_order_acquire);
    const std::uint64_t arena_now = m_arena_pos.load(std::memory_order_acquire);
    if (head_now == cursor) {
      return 0;  // closed with no pending AU
    }

    const au_slot& probe = m_aus[static_cast<std::size_t>(cursor) & m_slot_mask];
    if (head_now - cursor > k_max_aus
        || (arena_now - probe.arena_offset) > m_capacity)
    {
      cursor = apply_drop_oldest(slot, cursor, head_now, arena_now);
      if (cursor >= head_now) {
        return 0;
      }
    }

    const au_slot& au = m_aus[static_cast<std::size_t>(cursor) & m_slot_mask];
    const au_meta meta = au.meta;
    const std::uint64_t au_offset = au.arena_offset;
    const std::size_t size = meta.size;

    if (size == 0 || size > m_capacity) {
      // Cannot happen for a descriptor the producer published; treat a torn
      // descriptor as unrecoverable for this AU rather than copying garbage.
      slot.dropped.fetch_add(1, std::memory_order_relaxed);
      cursor += 1;
      slot.cursor.store(cursor, std::memory_order_release);
      continue;
    }

    if (size > cap) {
      // The caller's buffer cannot hold this whole AU; never split it. Skip the
      // undeliverable AU (counted as a drop) rather than stalling.
      slot.dropped.fetch_add(1, std::memory_order_relaxed);
      cursor += 1;
      slot.cursor.store(cursor, std::memory_order_release);
      continue;
    }

    const std::size_t off = static_cast<std::size_t>(au_offset) & m_mask;
    const std::size_t first = std::min(size, m_capacity - off);
    std::memcpy(dst, &m_buf[off], first);
    if (first < size) {
      std::memcpy(dst + first, m_buf.data(), size - first);
    }

    // Validation: if the producer recycled the descriptor or lapped the copied
    // AU mid-read, discard and retry (the retry applies drop-oldest).
    const std::uint64_t head_after = m_head_au.load(std::memory_order_acquire);
    const std::uint64_t arena_after =
        m_arena_pos.load(std::memory_order_acquire);
    if (head_after - cursor > k_max_aus
        || (arena_after - au_offset) > m_capacity)
    {
      continue;
    }

    out = meta;
    slot.cursor.store(cursor + 1, std::memory_order_release);
    return size;
  }
}

auto es_ring::dropped_aus(int con) const noexcept -> std::uint64_t
{
  if (con < 0 || con >= k_max_consumers) {
    return 0;
  }
  return m_slots[static_cast<std::size_t>(con)].dropped.load(
      std::memory_order_relaxed);
}

auto es_ring::close() -> void
{
  {
    std::lock_guard<std::mutex> guard(m_wake_mutex);
    m_closed.store(true, std::memory_order_release);
  }
  m_wake_cv.notify_all();
}
