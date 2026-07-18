// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/ring.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace
{
constexpr uint64_t k_bits_per_byte = 8;
constexpr uint64_t k_sizing_seconds = 10;
constexpr uint64_t k_kbps = 1000;
}  // namespace

auto ts_ring::size_for_bandwidth(int bandwidth_kbps) noexcept -> std::size_t
{
  const uint64_t bytes_per_second =
      static_cast<uint64_t>(std::max(bandwidth_kbps, 0)) * k_kbps
      / k_bits_per_byte;
  const uint64_t wanted = bytes_per_second * k_sizing_seconds;
  const uint64_t clamped = std::clamp<uint64_t>(wanted, k_min_bytes, k_max_bytes);
  return std::bit_ceil(clamped);  // k_max_bytes is a power of two: clamp holds
}

ts_ring::ts_ring(std::size_t capacity_bytes)
    : m_capacity {std::bit_ceil(
          std::clamp(capacity_bytes, k_min_bytes, k_max_bytes))}
    , m_mask {m_capacity - 1}
    , m_buf(m_capacity)
{
}

auto ts_ring::write(const uint8_t* data, std::size_t len) -> void
{
  if (data == nullptr || len == 0) {
    return;
  }
  if (len > m_capacity) {  // defensive; RIST payloads are ~1316 bytes
    data += len - m_capacity;
    len = m_capacity;
  }

  const uint64_t pos = m_write_pos.load(std::memory_order_relaxed);
  const std::size_t off = static_cast<std::size_t>(pos) & m_mask;
  const std::size_t first = std::min(len, m_capacity - off);
  std::memcpy(&m_buf[off], data, first);
  if (first < len) {
    std::memcpy(m_buf.data(), data + first, len - first);
  }
  m_write_pos.store(pos + len, std::memory_order_release);

  // Empty critical section pairs with the consumer's predicate check under the
  // same mutex: either the consumer saw the new head before sleeping, or it is
  // asleep and this notify wakes it. The producer never waits on consumers.
  {
    std::lock_guard<std::mutex> guard(m_wake_mutex);
  }
  m_wake_cv.notify_all();
}

auto ts_ring::add_consumer() -> int
{
  std::lock_guard<std::mutex> guard(m_slots_mutex);
  for (int idx = 0; idx < k_max_consumers; ++idx) {
    consumer_slot& slot = m_slots[static_cast<std::size_t>(idx)];
    if (!slot.active.load(std::memory_order_acquire)) {
      slot.cursor.store(m_write_pos.load(std::memory_order_acquire),
                        std::memory_order_release);
      slot.dropped.store(0, std::memory_order_release);
      slot.active.store(true, std::memory_order_release);
      return idx;
    }
  }
  return -1;
}

auto ts_ring::remove_consumer(int con) -> void
{
  if (con < 0 || con >= k_max_consumers) {
    return;
  }
  std::lock_guard<std::mutex> guard(m_slots_mutex);
  m_slots[static_cast<std::size_t>(con)].active.store(
      false, std::memory_order_release);
}

auto ts_ring::apply_drop_oldest(consumer_slot& slot,
                                uint64_t cursor,
                                uint64_t head_now) const -> uint64_t
{
  // Oldest byte still valid is head - capacity. Jump there, then round UP to
  // the next TS-packet boundary so the parser downstream resynchronises on a
  // packet start (the stream is 188-framed from absolute offset 0; if the
  // producer was ever fed unaligned data, tsparse's 0x47 sync recovers).
  uint64_t target = head_now - m_capacity;
  const uint64_t misalign = target % k_ts_packet;
  if (misalign != 0) {
    target += k_ts_packet - misalign;
  }
  if (target > head_now) {  // pathological tiny ring; cannot happen with 16 MiB
    target = head_now;
  }
  slot.dropped.fetch_add(target - cursor, std::memory_order_relaxed);
  slot.cursor.store(target, std::memory_order_release);
  return target;
}

auto ts_ring::read(int con,
                   uint8_t* dst,
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

  uint64_t cursor = slot.cursor.load(std::memory_order_acquire);

  // Wait for data (or closure). The predicate reads the head under the wakeup
  // mutex, pairing with the producer's empty critical section.
  {
    std::unique_lock<std::mutex> wait_lock(m_wake_mutex);
    const bool ready = m_wake_cv.wait_for(
        wait_lock,
        timeout,
        [&]() -> bool
        {
          return m_closed.load(std::memory_order_acquire)
              || m_write_pos.load(std::memory_order_acquire) > cursor;
        });
    if (!ready) {
      return 0;  // timeout
    }
  }

  // Seqlock-style copy: validate afterwards that the producer did not lap the
  // copied region; retry with a jumped cursor if it did.
  for (;;) {
    const uint64_t head_now = m_write_pos.load(std::memory_order_acquire);
    if (head_now == cursor) {
      return 0;  // closed with no pending data
    }
    if (head_now - cursor > m_capacity) {
      cursor = apply_drop_oldest(slot, cursor, head_now);
      if (head_now == cursor) {
        return 0;
      }
    }

    const std::size_t avail = static_cast<std::size_t>(
        std::min<uint64_t>(head_now - cursor, max_len));
    const std::size_t off = static_cast<std::size_t>(cursor) & m_mask;
    const std::size_t first = std::min(avail, m_capacity - off);
    std::memcpy(dst, &m_buf[off], first);
    if (first < avail) {
      std::memcpy(dst + first, m_buf.data(), avail - first);
    }

    // Validation: if the producer has advanced past cursor + capacity, part of
    // what we just copied may have been overwritten mid-copy. Discard + retry.
    const uint64_t head_after = m_write_pos.load(std::memory_order_acquire);
    if (head_after - cursor > m_capacity) {
      cursor = apply_drop_oldest(slot, cursor, head_after);
      continue;
    }

    slot.cursor.store(cursor + avail, std::memory_order_release);
    return avail;
  }
}

auto ts_ring::dropped_bytes(int con) const noexcept -> uint64_t
{
  if (con < 0 || con >= k_max_consumers) {
    return 0;
  }
  return m_slots[static_cast<std::size_t>(con)].dropped.load(
      std::memory_order_relaxed);
}

auto ts_ring::close() -> void
{
  {
    std::lock_guard<std::mutex> guard(m_wake_mutex);
    m_closed.store(true, std::memory_order_release);
  }
  m_wake_cv.notify_all();
}
