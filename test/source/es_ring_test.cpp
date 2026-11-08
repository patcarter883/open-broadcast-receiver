// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for es_ring — the compressed-elementary-stream ring of the shared
// standby stage. Dependency-free (no test framework): each check aborts with a
// message on failure; exit 0 means pass. Run via ctest. Deliberately free of
// GStreamer so it builds and runs in the tests-only lane.
//
// The property under test that ts_ring cannot offer: a consumer is only ever
// advanced to an ACCESS UNIT BOUNDARY. A partial AU handed to a decoder or a
// muxer is corruption, so every delivered buffer must be exactly the size its
// descriptor announced, with its payload intact.
//
// Mirrors test/source/frame_ring_test.cpp in structure and assertion style.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "fanout/es_ring.h"

namespace
{
constexpr std::chrono::milliseconds k_tick {200};
constexpr std::int64_t k_frame_ns = 33'333'333;  // 30 fps

auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// Deterministic payload pattern keyed by (AU index, byte offset) so any
// delivered AU can be validated by identity and position.
auto pattern_at(std::uint64_t au_idx, std::size_t byte) -> std::uint8_t
{
  return static_cast<std::uint8_t>(
      (au_idx * 131 + static_cast<std::uint64_t>(byte) * 7 + 3) & 0xFF);
}

// Deliberately uneven AU sizes (a real encoder's output is lumpy: an IDR is
// many times a P-frame), so a byte-lapping drop can never accidentally line up
// with a descriptor boundary.
auto size_for(std::uint64_t au_idx) -> std::size_t
{
  return 1500 + static_cast<std::size_t>((au_idx * 7919U) % 200000U);
}

auto make_meta(std::uint64_t au_idx) -> es_ring::au_meta
{
  es_ring::au_meta meta;
  meta.pts_ns = static_cast<std::int64_t>(au_idx) * k_frame_ns;
  meta.dts_ns = meta.pts_ns - k_frame_ns;
  meta.seq = au_idx;
  meta.keyframe = (au_idx % 30U) == 0U;
  return meta;
}

auto fill_au(std::vector<std::uint8_t>& buf, std::uint64_t au_idx) -> void
{
  for (std::size_t byte = 0; byte < buf.size(); ++byte) {
    buf[byte] = pattern_at(au_idx, byte);
  }
}

auto verify_au(const std::uint8_t* buf,
               std::size_t len,
               std::uint64_t au_idx) -> bool
{
  for (std::size_t byte = 0; byte < len; ++byte) {
    if (buf[byte] != pattern_at(au_idx, byte)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// construction/sizing: clamp to [k_min_bytes, k_max_bytes], power of two
// ---------------------------------------------------------------------------
auto test_construction() -> void
{
  es_ring tiny(1);
  expect(tiny.capacity() == es_ring::k_min_bytes, "min clamps to the floor");
  expect((tiny.capacity() & (tiny.capacity() - 1)) == 0, "power of two");
  expect(es_ring::k_max_aus >= 64, "enough descriptor slots for a GOP");

  es_ring mid(5UL * 1024 * 1024);
  expect(mid.capacity() == 8UL * 1024 * 1024, "5 MiB rounds up to 8 MiB");

  es_ring huge(1000000000UL);
  expect(huge.capacity() == es_ring::k_max_bytes, "capped at k_max_bytes");

  // Sized from the session bitrate: 10 s of an 8 Mbit/s stream is 10 MB, so
  // the ring must hold at least that (16 MiB after rounding).
  const std::size_t sized = es_ring::size_for_bps(8000);
  expect(sized >= 10UL * 1000 * 1000, "size_for_bps holds ten seconds");
  expect((sized & (sized - 1)) == 0, "size_for_bps is a power of two");
  expect(es_ring::size_for_bps(0) == es_ring::k_min_bytes,
         "unknown bitrate falls back to the floor");
  expect(es_ring::size_for_bps(1000000) == es_ring::k_max_bytes,
         "absurd bitrate clamps to the ceiling");

  std::puts("ok: construction");
}

// ---------------------------------------------------------------------------
// whole AUs of uneven size survive many laps of the arena
// ---------------------------------------------------------------------------
auto test_wrap() -> void
{
  es_ring ring(1);  // clamps to k_min_bytes
  const int con = ring.add_consumer();
  expect(con >= 0, "consumer slot");

  std::vector<std::uint8_t> wbuf;
  std::vector<std::uint8_t> rbuf(ring.capacity());

  // ~2x the arena: guarantees the producer wraps under a live reader.
  std::uint64_t written = 0;
  std::uint64_t read_aus = 0;
  std::size_t payload_bytes = 0;
  while (payload_bytes < 2UL * ring.capacity()) {
    wbuf.resize(size_for(written));
    fill_au(wbuf, written);
    es_ring::au_meta meta = make_meta(written);
    meta.size = wbuf.size();
    expect(ring.write(meta, wbuf.data(), wbuf.size()), "write accepted");
    payload_bytes += wbuf.size();
    ++written;

    // Drain fully each iteration -> the reader is never lapped.
    for (;;) {
      es_ring::au_meta got;
      const std::size_t size = ring.read(
          con, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (size == 0) {
        break;
      }
      expect(size == size_for(read_aus), "whole AU across wrap");
      expect(got.size == size, "descriptor size matches the payload");
      expect(got.pts_ns == static_cast<std::int64_t>(read_aus) * k_frame_ns,
             "pts intact across wrap");
      expect(got.dts_ns == got.pts_ns - k_frame_ns, "dts intact across wrap");
      expect(got.keyframe == (read_aus % 30U == 0U), "keyframe flag intact");
      expect(got.seq == read_aus, "sequence number is the AU index");
      expect(verify_au(rbuf.data(), size, read_aus), "payload intact");
      ++read_aus;
    }
  }
  expect(read_aus == written, "all AUs delivered");
  expect(ring.dropped_aus(con) == 0, "no drops when keeping up");
  std::puts("ok: wrap");
}

// ---------------------------------------------------------------------------
// a lapped consumer skips forward by WHOLE AUs and accounts every one
// ---------------------------------------------------------------------------
auto test_drop_oldest_whole_aus() -> void
{
  es_ring ring(1);  // k_min_bytes
  const std::size_t cap = ring.capacity();
  const int fast = ring.add_consumer();
  const int slow = ring.add_consumer();
  expect(fast >= 0 && slow >= 0, "two consumers");

  std::vector<std::uint8_t> wbuf;
  std::vector<std::uint8_t> rbuf(cap);

  // 100 AUs of uneven size, well past the arena so the slow consumer laps.
  constexpr std::uint64_t k_total = 100;
  std::size_t payload_bytes = 0;
  for (std::uint64_t idx = 0; idx < k_total; ++idx) {
    wbuf.resize(size_for(idx));
    fill_au(wbuf, idx);
    es_ring::au_meta meta = make_meta(idx);
    meta.size = wbuf.size();
    expect(ring.write(meta, wbuf.data(), wbuf.size()), "write accepted");
    payload_bytes += wbuf.size();

    // The fast consumer keeps up frame for frame.
    for (;;) {
      es_ring::au_meta got;
      const std::size_t size = ring.read(
          fast, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (size == 0) {
        break;
      }
      expect(size == size_for(got.seq), "fast consumer gets whole AUs");
      expect(verify_au(rbuf.data(), size, got.seq), "fast payload valid");
    }
  }
  expect(payload_bytes > 2UL * cap, "the test really laps the arena");
  expect(ring.dropped_aus(fast) == 0, "fast consumer dropped nothing");

  // The slow consumer wakes: the first AU it sees must be whole and its index
  // must equal exactly the number of AUs it skipped.
  es_ring::au_meta first;
  const std::size_t got_size =
      ring.read(slow, first, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
  const std::uint64_t dropped = ring.dropped_aus(slow);
  expect(got_size == size_for(first.seq), "slow AU is whole (never mid-AU)");
  expect(first.size == got_size, "slow AU descriptor matches payload size");
  expect(dropped > 0, "slow consumer recorded drops");
  expect(first.seq == dropped, "cursor lands on an AU boundary, no AU lost twice");
  expect(verify_au(rbuf.data(), got_size, first.seq), "slow AU payload valid");

  // Drain and check exact whole-AU accounting.
  std::uint64_t slow_aus = 1;
  for (;;) {
    es_ring::au_meta got;
    const std::size_t size = ring.read(
        slow, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
    if (size == 0) {
      break;
    }
    expect(size == got.size, "every delivered AU is whole");
    expect(got.seq == dropped + slow_aus, "AUs are delivered in order, no gaps");
    expect(verify_au(rbuf.data(), size, got.seq), "drained AU payload valid");
    ++slow_aus;
  }
  expect(ring.dropped_aus(slow) == dropped, "drop count stable after drain");
  expect(dropped + slow_aus == k_total,
         "dropped + delivered == written (exact AU accounting)");
  std::puts("ok: drop-oldest (whole AUs)");
}

// ---------------------------------------------------------------------------
// two consumers at different speeds both make progress
// ---------------------------------------------------------------------------
auto test_two_speeds_progress() -> void
{
  es_ring ring(1);
  const int fast = ring.add_consumer();
  const int slow = ring.add_consumer();
  expect(fast >= 0 && slow >= 0, "two consumers");

  std::vector<std::uint8_t> wbuf;
  std::vector<std::uint8_t> rbuf(ring.capacity());

  constexpr std::uint64_t k_total = 120;
  std::uint64_t fast_aus = 0;
  std::uint64_t slow_aus = 0;
  for (std::uint64_t idx = 0; idx < k_total; ++idx) {
    wbuf.resize(size_for(idx));
    fill_au(wbuf, idx);
    es_ring::au_meta meta = make_meta(idx);
    meta.size = wbuf.size();
    expect(ring.write(meta, wbuf.data(), wbuf.size()), "write accepted");

    // Fast: every AU.
    for (;;) {
      es_ring::au_meta got;
      const std::size_t size = ring.read(
          fast, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (size == 0) {
        break;
      }
      expect(size == got.size, "fast AU whole");
      ++fast_aus;
    }

    // Slow: only every third AU is consumed -> it falls behind and is lapped,
    // but each read is still a whole AU.
    if (idx % 3U == 2U) {
      es_ring::au_meta got;
      const std::size_t size = ring.read(
          slow, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (size != 0) {
        expect(size == got.size, "slow AU whole");
        ++slow_aus;
      }
    }
  }
  expect(fast_aus == k_total, "fast consumer saw every AU");
  expect(slow_aus > 0, "slow consumer still delivers AUs");
  expect(ring.dropped_aus(slow) > 0, "slow consumer was lapped");
  expect(slow_aus + ring.dropped_aus(slow) <= k_total,
         "slow accounting is bounded by what was written");

  // The slow consumer drains its remaining backlog to the head.
  for (;;) {
    es_ring::au_meta got;
    const std::size_t size = ring.read(
        slow, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
    if (size == 0) {
      break;
    }
    expect(size == got.size, "drained AU whole");
    ++slow_aus;
  }
  expect(slow_aus + ring.dropped_aus(slow) == k_total,
         "both consumers account for every AU");
  std::puts("ok: two speeds progress");
}

// ---------------------------------------------------------------------------
// empty / null / oversized AUs are refused whole, head untouched
// ---------------------------------------------------------------------------
auto test_drop_whole() -> void
{
  es_ring ring(1);
  const int con = ring.add_consumer();
  std::vector<std::uint8_t> wbuf(4096, 0x33);

  expect(!ring.write(make_meta(0), wbuf.data(), 0), "empty AU rejected");
  expect(!ring.write(make_meta(0), nullptr, 10), "null payload rejected");
  expect(!ring.write(make_meta(0), wbuf.data(), ring.capacity() + 1),
         "oversized AU rejected (cannot be held whole)");
  expect(ring.head() == 0, "rejected AUs do not advance the head");

  expect(ring.write(make_meta(1), wbuf.data(), wbuf.size()), "valid write");
  std::vector<std::uint8_t> rbuf(ring.capacity());
  es_ring::au_meta got;
  const std::size_t size =
      ring.read(con, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
  expect(size == wbuf.size(), "following valid AU delivered whole");
  expect(got.pts_ns == k_frame_ns, "following AU keeps its stamps");
  std::puts("ok: drop whole");
}

// ---------------------------------------------------------------------------
// a consumer buffer too small for an AU skips that AU whole (never truncates)
// ---------------------------------------------------------------------------
auto test_caller_buffer_too_small() -> void
{
  es_ring ring(1);
  const int con = ring.add_consumer();

  std::vector<std::uint8_t> wbuf;
  for (std::uint64_t idx = 0; idx < 4; ++idx) {
    wbuf.resize(size_for(idx));
    fill_au(wbuf, idx);
    es_ring::au_meta meta = make_meta(idx);
    meta.size = wbuf.size();
    expect(ring.write(meta, wbuf.data(), wbuf.size()), "write");
  }

  std::vector<std::uint8_t> small(1024);  // fits none of the AUs
  es_ring::au_meta got;
  const std::size_t none = ring.read(
      con, got, small.data(), small.size(), std::chrono::milliseconds(0));
  expect(none == 0, "undeliverable AUs are skipped, not truncated");
  expect(ring.dropped_aus(con) == 4, "each skipped AU is accounted");

  wbuf.resize(size_for(4));
  fill_au(wbuf, 4);
  es_ring::au_meta meta = make_meta(4);
  meta.size = wbuf.size();
  expect(ring.write(meta, wbuf.data(), wbuf.size()), "write");
  std::vector<std::uint8_t> big(ring.capacity());
  const std::size_t size =
      ring.read(con, got, big.data(), big.size(), std::chrono::milliseconds(0));
  expect(size == size_for(4), "a fitting AU is delivered whole afterwards");
  std::puts("ok: caller buffer too small");
}

// ---------------------------------------------------------------------------
// producer never blocks on a stalled consumer
// ---------------------------------------------------------------------------
auto test_producer_never_blocks() -> void
{
  es_ring ring(1);
  const int con = ring.add_consumer();
  (void)con;  // never read: permanently stalled consumer

  std::vector<std::uint8_t> wbuf;
  const auto begin = std::chrono::steady_clock::now();
  std::size_t payload_bytes = 0;
  std::uint64_t idx = 0;
  while (payload_bytes < 8UL * ring.capacity()) {
    wbuf.resize(size_for(idx));
    es_ring::au_meta meta = make_meta(idx);
    meta.size = wbuf.size();
    expect(ring.write(meta, wbuf.data(), wbuf.size()), "write");
    payload_bytes += wbuf.size();
    ++idx;
  }
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  expect(elapsed < std::chrono::seconds(5), "producer completed unhindered");
  std::puts("ok: producer never blocks");
}

// ---------------------------------------------------------------------------
// close() wakes a blocked reader
// ---------------------------------------------------------------------------
auto test_close_wakes() -> void
{
  es_ring ring(1);
  const int con = ring.add_consumer();
  std::vector<std::uint8_t> rbuf(1024);

  std::thread closer(
      [&ring]() -> void
      {
        std::this_thread::sleep_for(k_tick);
        ring.close();
      });
  es_ring::au_meta got;
  const auto begin = std::chrono::steady_clock::now();
  const std::size_t size =
      ring.read(con, got, rbuf.data(), rbuf.size(), std::chrono::seconds(30));
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  closer.join();
  expect(size == 0, "close returns no AU");
  expect(elapsed < std::chrono::seconds(10), "close woke the reader early");
  std::puts("ok: close wakes");
}
}  // namespace

auto main() -> int
{
  test_construction();
  test_wrap();
  test_drop_oldest_whole_aus();
  test_two_speeds_progress();
  test_drop_whole();
  test_caller_buffer_too_small();
  test_producer_never_blocks();
  test_close_wakes();
  std::puts("es_ring_test: ALL OK");
  return 0;
}
