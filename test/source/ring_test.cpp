// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for ts_ring (TRANSPORT_PROFILE.md §3.4, FIXPLAN M1.3).
// Dependency-free (no test framework): each check aborts with a message on
// failure; exit 0 means pass. Run via ctest.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "fanout/ring.h"

namespace
{
constexpr std::chrono::milliseconds k_tick {200};

auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// Deterministic byte pattern so any slice can be validated by position.
auto pattern_at(uint64_t pos) -> uint8_t
{
  return static_cast<uint8_t>((pos * 31 + 7) & 0xFF);
}

auto fill_pattern(std::vector<uint8_t>& buf, uint64_t start) -> void
{
  for (std::size_t idx = 0; idx < buf.size(); ++idx) {
    buf[idx] = pattern_at(start + idx);
  }
}

auto verify_pattern(const uint8_t* buf, std::size_t len, uint64_t start) -> bool
{
  for (std::size_t idx = 0; idx < len; ++idx) {
    if (buf[idx] != pattern_at(start + idx)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// sizing: clamp(10 s × bandwidth, 16 MiB, 256 MiB), power-of-two
// ---------------------------------------------------------------------------
auto test_sizing() -> void
{
  // 6 Mbps default → 7.5 MB wanted → clamped up to 16 MiB
  expect(ts_ring::size_for_bandwidth(6000) == 16UL * 1024 * 1024,
         "6 Mbps clamps to 16 MiB floor");
  // 100 Mbps → 125 MB → bit_ceil → 128 MiB
  expect(ts_ring::size_for_bandwidth(100000) == 128UL * 1024 * 1024,
         "100 Mbps sizes to 128 MiB");
  // absurd input → 256 MiB cap
  expect(ts_ring::size_for_bandwidth(10000000) == 256UL * 1024 * 1024,
         "cap at 256 MiB");
  expect(ts_ring::size_for_bandwidth(0) == 16UL * 1024 * 1024,
         "0 bandwidth clamps to floor");
  std::puts("ok: sizing");
}

// ---------------------------------------------------------------------------
// wrap correctness: continuous pattern survives many laps of a small ring
// ---------------------------------------------------------------------------
auto test_wrap() -> void
{
  ts_ring ring(1);  // clamps to 16 MiB
  const int con = ring.add_consumer();
  expect(con >= 0, "consumer slot");

  const std::size_t chunk = 7 * ts_ring::k_ts_packet;  // 1316: RIST payload
  const uint64_t total = 40UL * 1024 * 1024;           // 2.5 laps
  std::vector<uint8_t> wbuf(chunk);
  std::vector<uint8_t> rbuf(64 * 1024);

  uint64_t written = 0;
  uint64_t read_pos = 0;
  while (written < total) {
    fill_pattern(wbuf, written);
    ring.write(wbuf.data(), wbuf.size());
    written += wbuf.size();

    // Drain fully each iteration → reader is never lapped.
    for (;;) {
      const std::size_t got =
          ring.read(con, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (got == 0) {
        break;
      }
      expect(verify_pattern(rbuf.data(), got, read_pos),
             "pattern intact across wrap");
      read_pos += got;
    }
  }
  expect(read_pos == written, "all bytes delivered");
  expect(ring.dropped_bytes(con) == 0, "no drops when keeping up");
  std::puts("ok: wrap");
}

// ---------------------------------------------------------------------------
// cursor independence + drop-oldest accounting at a TS boundary
// ---------------------------------------------------------------------------
auto test_independent_cursors_and_drop() -> void
{
  ts_ring ring(1);  // 16 MiB
  const std::size_t cap = ring.capacity();
  const int fast = ring.add_consumer();
  const int slow = ring.add_consumer();
  expect(fast >= 0 && slow >= 0, "two consumers");

  const std::size_t chunk = 7 * ts_ring::k_ts_packet;
  std::vector<uint8_t> wbuf(chunk);
  std::vector<uint8_t> rbuf(256 * 1024);

  // Write 1.5 rings worth. The fast consumer drains as we go; slow stalls.
  const uint64_t total = cap + cap / 2;
  uint64_t written = 0;
  uint64_t fast_pos = 0;
  while (written < total) {
    fill_pattern(wbuf, written);
    ring.write(wbuf.data(), wbuf.size());
    written += wbuf.size();
    for (;;) {
      const std::size_t got = ring.read(
          fast, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (got == 0) {
        break;
      }
      expect(verify_pattern(rbuf.data(), got, fast_pos), "fast pattern");
      fast_pos += got;
    }
  }
  expect(fast_pos == written, "fast consumer saw everything");
  expect(ring.dropped_bytes(fast) == 0, "fast consumer dropped nothing");

  // Slow consumer wakes: must have been advanced (drop-oldest), land on a
  // 188 boundary, have exact accounting, and read a valid pattern from there.
  const std::size_t got =
      ring.read(slow, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
  expect(got > 0, "slow consumer gets data after stall");
  const uint64_t dropped = ring.dropped_bytes(slow);
  expect(dropped > 0, "slow consumer recorded drops");
  expect(dropped % ts_ring::k_ts_packet == 0
             || (dropped % ts_ring::k_ts_packet)
                 == (written - cap) % ts_ring::k_ts_packet,
         "drop advance respects packet alignment rule");
  // The cursor after the drop is `dropped` (started at 0); verify content.
  expect(verify_pattern(rbuf.data(), got, dropped), "slow reads valid bytes");
  expect(dropped % ts_ring::k_ts_packet == 0, "cursor landed on 188 boundary");
  expect(dropped >= written - cap, "dropped at least the overrun");
  std::puts("ok: cursors+drop");
}

// ---------------------------------------------------------------------------
// M1.3 acceptance: at simulated 100 Mbps the ring sustains a 9 s consumer
// stall with zero drops; an 11 s stall drops with correct accounting.
// ---------------------------------------------------------------------------
auto stall_case(uint64_t stall_seconds, bool expect_drops) -> void
{
  const int bandwidth_kbps = 100000;  // 100 Mbps
  ts_ring ring(ts_ring::size_for_bandwidth(bandwidth_kbps));  // 128 MiB
  const int con = ring.add_consumer();

  const uint64_t bytes_per_second =
      static_cast<uint64_t>(bandwidth_kbps) * 1000 / 8;  // 12.5 MB/s
  const uint64_t stall_bytes = bytes_per_second * stall_seconds;

  const std::size_t chunk = 7 * ts_ring::k_ts_packet;
  std::vector<uint8_t> wbuf(chunk);
  std::vector<uint8_t> rbuf(1024 * 1024);

  // Consumer is stalled: write stall_seconds worth of traffic unread.
  uint64_t written = 0;
  while (written < stall_bytes) {
    fill_pattern(wbuf, written);
    ring.write(wbuf.data(), wbuf.size());
    written += wbuf.size();
  }

  // Consumer resumes and drains.
  uint64_t delivered = 0;
  uint64_t first_pos = UINT64_MAX;
  for (;;) {
    const std::size_t got =
        ring.read(con, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
    if (got == 0) {
      break;
    }
    if (first_pos == UINT64_MAX) {
      first_pos = ring.dropped_bytes(con);  // cursor started at 0
      expect(verify_pattern(rbuf.data(), got, first_pos),
             "resume reads valid bytes");
    }
    delivered += got;
  }

  const uint64_t dropped = ring.dropped_bytes(con);
  if (expect_drops) {
    expect(dropped > 0, "long stall must drop");
    expect(dropped % ts_ring::k_ts_packet == 0, "drop lands on TS boundary");
    expect(dropped + delivered == written,
           "dropped + delivered == written (exact accounting)");
    expect(dropped >= written - ring.capacity(), "dropped covers the overrun");
  } else {
    expect(dropped == 0, "9 s stall at 100 Mbps must not drop");
    expect(delivered == written, "everything delivered after short stall");
  }
}

auto test_stalls() -> void
{
  stall_case(9, /*expect_drops=*/false);
  stall_case(11, /*expect_drops=*/true);
  std::puts("ok: 9s/11s stall (M1.3 acceptance)");
}

// ---------------------------------------------------------------------------
// producer never blocks: writer completes a fixed volume while a consumer
// sleeps holding nothing; wall time bounded (no consumer-paced waits).
// ---------------------------------------------------------------------------
auto test_producer_never_blocks() -> void
{
  ts_ring ring(1);
  const int con = ring.add_consumer();
  (void)con;  // never read from: permanently stalled consumer

  const std::size_t chunk = 7 * ts_ring::k_ts_packet;
  std::vector<uint8_t> wbuf(chunk, 0xAB);

  const auto begin = std::chrono::steady_clock::now();
  const uint64_t total = 64UL * 1024 * 1024;  // 4 laps of a stalled consumer
  for (uint64_t written = 0; written < total; written += chunk) {
    ring.write(wbuf.data(), wbuf.size());
  }
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  // Generous bound: pure memcpy of 64 MiB takes well under a second; if the
  // producer ever waited on the stalled consumer this explodes.
  expect(elapsed < std::chrono::seconds(5), "producer completed unhindered");
  std::puts("ok: producer never blocks");
}

// ---------------------------------------------------------------------------
// close() wakes a blocked consumer
// ---------------------------------------------------------------------------
auto test_close_wakes() -> void
{
  ts_ring ring(1);
  const int con = ring.add_consumer();
  std::vector<uint8_t> rbuf(1024);

  std::thread closer(
      [&ring]() -> void
      {
        std::this_thread::sleep_for(k_tick);
        ring.close();
      });
  const auto begin = std::chrono::steady_clock::now();
  const std::size_t got =
      ring.read(con, rbuf.data(), rbuf.size(), std::chrono::seconds(30));
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  closer.join();
  expect(got == 0, "close returns no data");
  expect(elapsed < std::chrono::seconds(10), "close woke the reader early");
  std::puts("ok: close wakes");
}
}  // namespace

auto main() -> int
{
  test_sizing();
  test_wrap();
  test_independent_cursors_and_drop();
  test_stalls();
  test_producer_never_blocks();
  test_close_wakes();
  std::puts("ring_test: ALL OK");
  return 0;
}
