// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/frame_ring.h"

#include <algorithm>
#include <bit>
#include <cstring>

frame_ring::frame_ring(std::size_t capacity_bytes)
    : m_capacity {std::bit_ceil(
          std::clamp(capacity_bytes, k_min_bytes, k_max_bytes))}
    , m_mask {m_capacity - 1}
    , m_buf(m_capacity)
{
}

auto frame_ring::write(const frame_meta& meta,
                       const std::uint8_t* data,
                       std::size_t len) -> bool
{
  // A frame is delivered whole or not at all: an empty or oversized payload is
  // dropped without touching the ring (a truncated raw frame is garbage to any
  // decoder/encoder downstream). The producer never blocks.
  if (data == nullptr || len == 0 || len > m_capacity) {
    return false;
  }

  const std::uint64_t arena_pos = m_arena_pos.load(std::memory_order_relaxed);
  const std::uint64_t frame_idx = m_head_frame.load(std::memory_order_relaxed);
  const std::size_t off = static_cast<std::size_t>(arena_pos) & m_mask;
  const std::size_t first = std::min(len, m_capacity - off);
  std::memcpy(&m_buf[off], data, first);
  if (first < len) {
    std::memcpy(m_buf.data(), data + first, len - first);
  }

  frame_slot& slot =
      m_frames[static_cast<std::size_t>(frame_idx) & m_frame_mask];
  slot.meta = meta;
  slot.meta.size = static_cast<std::uint32_t>(len);
  slot.arena_offset = arena_pos;

  // Publish the payload position first, then the new head: the head release
  // store is the single publication point a consumer acquire-loads, and it
  // orders the descriptor store and the byte copies before it.
  m_arena_pos.store(arena_pos + len, std::memory_order_release);
  m_head_frame.store(frame_idx + 1, std::memory_order_release);

  // Empty critical section pairs with the consumer's predicate check under the
  // same mutex: either the consumer saw the new head before sleeping, or it is
  // asleep and this notify wakes it. The producer never waits on consumers.
  {
    std::lock_guard<std::mutex> guard(m_wake_mutex);
  }
  m_wake_cv.notify_all();
  return true;
}

auto frame_ring::add_consumer() -> int
{
  std::lock_guard<std::mutex> guard(m_slots_mutex);
  for (int idx = 0; idx < k_max_consumers; ++idx) {
    consumer_slot& slot = m_slots[static_cast<std::size_t>(idx)];
    if (!slot.active.load(std::memory_order_acquire)) {
      slot.cursor.store(m_head_frame.load(std::memory_order_acquire),
                        std::memory_order_release);
      slot.dropped.store(0, std::memory_order_release);
      slot.active.store(true, std::memory_order_release);
      return idx;
    }
  }
  return -1;
}

auto frame_ring::remove_consumer(int con) -> void
{
  if (con < 0 || con >= k_max_consumers) {
    return;
  }
  std::lock_guard<std::mutex> guard(m_slots_mutex);
  m_slots[static_cast<std::size_t>(con)].active.store(
      false, std::memory_order_release);
}

auto frame_ring::apply_drop_oldest(consumer_slot& slot,
                                   std::uint64_t cursor,
                                   std::uint64_t head_now,
                                   std::uint64_t arena_now) const -> std::uint64_t
{
  std::uint64_t dropped = 0;

  // Descriptor-slot lapping: a cursor more than k_max_frames behind the head
  // points at a slot a newer frame has already reused. Jump to the oldest frame
  // whose slot is still intact (this also bounds the byte scan below).
  if (head_now - cursor > k_max_frames) {
    const std::uint64_t jump = head_now - k_max_frames - cursor;
    cursor += jump;
    dropped += jump;
  }

  // Byte lapping: the arena retains only the last capacity bytes, so skip every
  // frame whose payload the producer has already begun to overwrite. After the
  // slot clamp there are at most k_max_frames frames to inspect.
  while (cursor < head_now
         && (arena_now
             - m_frames[static_cast<std::size_t>(cursor) & m_frame_mask]
                   .arena_offset)
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

auto frame_ring::read(int con,
                      frame_meta& out,
                      std::uint8_t* dst,
                      std::size_t max_len,
                      std::chrono::milliseconds timeout) -> std::size_t
{
  if (con < 0 || con >= k_max_consumers || dst == nullptr || max_len == 0) {
    return 0;
  }
  consumer_slot& slot = m_slots[static_cast<std::size_t>(con)];
  if (!slot.active.load(std::memory_order_acquire)) {
    return 0;
  }

  std::uint64_t cursor = slot.cursor.load(std::memory_order_acquire);

  // Wait for a frame (or closure). The predicate reads the head under the
  // wakeup mutex, pairing with the producer's empty critical section.
  {
    std::unique_lock<std::mutex> wait_lock(m_wake_mutex);
    const bool ready = m_wake_cv.wait_for(
        wait_lock,
        timeout,
        [&]() -> bool
        {
          return m_closed.load(std::memory_order_acquire)
              || m_head_frame.load(std::memory_order_acquire) > cursor;
        });
    if (!ready) {
      return 0;  // timeout
    }
  }

  // Seqlock-style copy: validate afterwards that the producer did not recycle
  // the descriptor slot or overwrite the frame; retry with a jumped cursor if
  // it did.
  for (;;) {
    const std::uint64_t head_now = m_head_frame.load(std::memory_order_acquire);
    const std::uint64_t arena_now = m_arena_pos.load(std::memory_order_acquire);
    if (head_now == cursor) {
      return 0;  // closed with no pending frame
    }

    const frame_slot& probe =
        m_frames[static_cast<std::size_t>(cursor) & m_frame_mask];
    if (head_now - cursor > k_max_frames
        || (arena_now - probe.arena_offset) > m_capacity)
    {
      cursor = apply_drop_oldest(slot, cursor, head_now, arena_now);
      if (cursor >= head_now) {
        return 0;
      }
    }

    const frame_slot& frame =
        m_frames[static_cast<std::size_t>(cursor) & m_frame_mask];
    const std::size_t size = frame.meta.size;
    if (size > max_len) {
      // The caller's buffer cannot hold this whole frame; never split it. Skip
      // the undeliverable frame (counted as a drop) rather than stalling.
      slot.dropped.fetch_add(1, std::memory_order_relaxed);
      cursor += 1;
      slot.cursor.store(cursor, std::memory_order_release);
      continue;
    }

    const std::size_t off =
        static_cast<std::size_t>(frame.arena_offset) & m_mask;
    const std::size_t first = std::min(size, m_capacity - off);
    std::memcpy(dst, &m_buf[off], first);
    if (first < size) {
      std::memcpy(dst + first, m_buf.data(), size - first);
    }
    const frame_meta meta = frame.meta;

    // Validation: if the producer recycled the slot or lapped the copied frame
    // mid-read, discard and retry (the retry applies drop-oldest).
    const std::uint64_t head_after = m_head_frame.load(std::memory_order_acquire);
    const std::uint64_t arena_after =
        m_arena_pos.load(std::memory_order_acquire);
    if (head_after - cursor > k_max_frames
        || (arena_after - frame.arena_offset) > m_capacity)
    {
      continue;
    }

    out = meta;
    slot.cursor.store(cursor + 1, std::memory_order_release);
    return size;
  }
}

auto frame_ring::dropped_frames(int con) const noexcept -> std::uint64_t
{
  if (con < 0 || con >= k_max_consumers) {
    return 0;
  }
  return m_slots[static_cast<std::size_t>(con)].dropped.load(
      std::memory_order_relaxed);
}

auto frame_ring::close() -> void
{
  {
    std::lock_guard<std::mutex> guard(m_wake_mutex);
    m_closed.store(true, std::memory_order_release);
  }
  m_wake_cv.notify_all();
}
