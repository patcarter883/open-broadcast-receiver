// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for frame_ring — the SPMC whole-frame hand-off of the opt-in
// transcode tier. Dependency-free (no test framework): each check aborts with a
// message on failure; exit 0 means pass. Run via ctest. Deliberately free of
// GStreamer/librist so it builds and runs in the tests-only lane.
//
// Mirrors test/source/ring_test.cpp in structure and assertion style.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "fanout/frame_ring.h"

namespace
{
constexpr std::chrono::milliseconds k_tick {200};
constexpr std::size_t k_frame_bytes = 300000;  // ~1080p raw-ish frame

auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// Deterministic payload pattern keyed by (frame index, byte offset) so any
// delivered frame can be validated by position and identity.
auto pattern_at(std::uint64_t frame_idx, std::size_t byte) -> std::uint8_t
{
  return static_cast<std::uint8_t>(
      (frame_idx * 131 + static_cast<std::uint64_t>(byte) * 7 + 3) & 0xFF);
}

auto make_meta(std::uint64_t frame_idx) -> frame_ring::frame_meta
{
  frame_ring::frame_meta meta;
  meta.pts = frame_idx * 1000;
  meta.dts = frame_idx * 1000 - 1000;
  meta.flags = (frame_idx % 7 == 0) ? frame_ring::k_flag_keyframe : 0U;
  meta.format = 32315659;  // arbitrary opaque id (no GStreamer types here)
  meta.stride = 1920;
  return meta;
}

auto fill_frame(std::vector<std::uint8_t>& buf, std::uint64_t frame_idx)
    -> void
{
  for (std::size_t byte = 0; byte < buf.size(); ++byte) {
    buf[byte] = pattern_at(frame_idx, byte);
  }
}

auto verify_frame(const std::uint8_t* buf,
                  std::size_t len,
                  std::uint64_t frame_idx) -> bool
{
  for (std::size_t byte = 0; byte < len; ++byte) {
    if (buf[byte] != pattern_at(frame_idx, byte)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// construction/sizing: clamp to [16 MiB, 256 MiB], round up to a power of two
// ---------------------------------------------------------------------------
auto test_construction() -> void
{
  frame_ring tiny(1);
  expect(tiny.capacity() == 16UL * 1024 * 1024, "min clamps to 16 MiB");
  expect(tiny.max_frames() == frame_ring::k_max_frames, "frame slot count");
  expect((tiny.capacity() & (tiny.capacity() - 1)) == 0, "power of two");

  frame_ring mid(50UL * 1024 * 1024);
  expect(mid.capacity() == 64UL * 1024 * 1024, "50 MiB rounds up to 64 MiB");

  frame_ring huge(1000000000UL);
  expect(huge.capacity() == 256UL * 1024 * 1024, "cap at 256 MiB");

  frame_ring zero(0);
  expect(zero.capacity() == 16UL * 1024 * 1024, "0 clamps to floor");
  std::puts("ok: construction");
}

// ---------------------------------------------------------------------------
// wrap correctness: whole frames with their metadata survive many laps
// ---------------------------------------------------------------------------
auto test_wrap() -> void
{
  frame_ring ring(1);  // clamps to 16 MiB
  const int con = ring.add_consumer();
  expect(con >= 0, "consumer slot");

  std::vector<std::uint8_t> wbuf(k_frame_bytes);
  std::vector<std::uint8_t> rbuf(ring.capacity());

  const std::uint64_t total_frames = 80;  // 24 MB payload: laps 16 MiB once
  std::uint64_t written = 0;
  std::uint64_t read_frames = 0;
  while (written < total_frames) {
    fill_frame(wbuf, written);
    expect(ring.write(make_meta(written), wbuf.data(), wbuf.size()),
           "write accepted");
    ++written;

    // Drain fully each iteration -> the reader is never lapped.
    for (;;) {
      frame_ring::frame_meta got;
      const std::size_t size = ring.read(
          con, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (size == 0) {
        break;
      }
      expect(size == k_frame_bytes, "whole frame size");
      expect(got.pts == read_frames * 1000, "pts intact across wrap");
      expect(got.dts == read_frames * 1000 - 1000, "dts intact across wrap");
      expect(got.flags == make_meta(read_frames).flags, "flags intact");
      expect(got.format == 32315659 && got.stride == 1920,
             "format/stride intact");
      expect(verify_frame(rbuf.data(), size, read_frames),
             "payload intact across wrap");
      ++read_frames;
    }
  }
  expect(read_frames == written, "all frames delivered");
  expect(ring.dropped_frames(con) == 0, "no drops when keeping up");
  std::puts("ok: wrap");
}

// ---------------------------------------------------------------------------
// per-consumer drop-oldest at WHOLE-FRAME boundaries (byte lapping)
// ---------------------------------------------------------------------------
auto test_drop_oldest_whole_frames() -> void
{
  frame_ring ring(1);  // 16 MiB
  const std::size_t cap = ring.capacity();
  const int fast = ring.add_consumer();
  const int slow = ring.add_consumer();
  expect(fast >= 0 && slow >= 0, "two consumers");

  std::vector<std::uint8_t> wbuf(k_frame_bytes);
  std::vector<std::uint8_t> rbuf(cap);

  const std::uint64_t total_frames = 80;  // 24 MB > 16 MiB: slow consumer laps
  std::uint64_t written = 0;
  std::uint64_t fast_frames = 0;
  while (written < total_frames) {
    fill_frame(wbuf, written);
    expect(ring.write(make_meta(written), wbuf.data(), wbuf.size()),
           "write accepted");
    ++written;
    for (;;) {
      frame_ring::frame_meta got;
      const std::size_t size = ring.read(
          fast, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      if (size == 0) {
        break;
      }
      expect(verify_frame(rbuf.data(), size, fast_frames), "fast payload");
      ++fast_frames;
    }
  }
  expect(fast_frames == written, "fast consumer saw every frame");
  expect(ring.dropped_frames(fast) == 0, "fast consumer dropped nothing");

  // The slow consumer wakes. It must have been advanced by whole frames: the
  // first frame it receives is exactly `dropped_frames` in, delivered whole.
  frame_ring::frame_meta first;
  const std::size_t got =
      ring.read(slow, first, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
  const std::uint64_t dropped = ring.dropped_frames(slow);
  expect(got == k_frame_bytes, "slow frame is whole (never mid-frame)");
  expect(dropped > 0, "slow consumer recorded drops");
  expect(first.pts == dropped * 1000, "cursor lands on a whole-frame boundary");
  expect(verify_frame(rbuf.data(), got, dropped), "slow reads a valid frame");

  // Drain the rest and check exact whole-frame accounting.
  std::uint64_t slow_frames = 1;  // the one just read
  for (;;) {
    frame_ring::frame_meta got_meta;
    const std::size_t size = ring.read(
        slow, got_meta, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
    if (size == 0) {
      break;
    }
    expect(size == k_frame_bytes, "every delivered frame is whole");
    expect(verify_frame(rbuf.data(), size, dropped + slow_frames),
           "drained frame valid");
    ++slow_frames;
  }
  expect(ring.dropped_frames(slow) == dropped, "drop count stable after drain");
  expect(dropped + slow_frames == written,
         "dropped + delivered == written (exact whole-frame accounting)");
  expect(dropped >= written - cap / k_frame_bytes - 1,
         "dropped covers the byte overrun");
  std::puts("ok: drop-oldest (whole frames)");
}

// ---------------------------------------------------------------------------
// descriptor-slot lapping: > k_max_frames unread frames with tiny payloads
// ---------------------------------------------------------------------------
auto test_slot_lap() -> void
{
  frame_ring ring(1);  // 16 MiB arena: 600 x 700 B never laps by bytes
  const int con = ring.add_consumer();
  const std::size_t small = 700;
  std::vector<std::uint8_t> wbuf(small, 0x5A);

  const std::uint64_t total_frames = frame_ring::k_max_frames + 88;
  for (std::uint64_t idx = 0; idx < total_frames; ++idx) {
    expect(ring.write(make_meta(idx), wbuf.data(), wbuf.size()), "write");
  }

  std::vector<std::uint8_t> rbuf(ring.capacity());
  frame_ring::frame_meta got;
  const std::size_t size =
      ring.read(con, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
  expect(size == small, "slot-lapped frame is whole");
  const std::uint64_t expected_drop = total_frames - frame_ring::k_max_frames;
  expect(ring.dropped_frames(con) == expected_drop,
         "slot lapping drops oldest by whole frames");
  expect(got.pts == expected_drop * 1000, "cursor landed on oldest intact slot");
  std::puts("ok: slot lap");
}

// ---------------------------------------------------------------------------
// cursor independence + a late joiner never sees pre-attach backlog
// ---------------------------------------------------------------------------
auto test_cursor_independence() -> void
{
  frame_ring ring(1);
  const int a = ring.add_consumer();
  std::vector<std::uint8_t> wbuf(k_frame_bytes, 0x11);
  std::vector<std::uint8_t> rbuf(ring.capacity());

  for (std::uint64_t idx = 0; idx < 3; ++idx) {
    expect(ring.write(make_meta(idx), wbuf.data(), wbuf.size()), "write");
  }
  const int b = ring.add_consumer();  // joins at head, after 3 frames

  // A reads from 0; B (cursor at head) reads nothing yet.
  for (std::uint64_t idx = 0; idx < 3; ++idx) {
    frame_ring::frame_meta got;
    const std::size_t size = ring.read(
        a, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
    expect(size == k_frame_bytes, "A reads its 3 frames");
    expect(got.pts == idx * 1000, "A independent cursor");
  }
  frame_ring::frame_meta none;
  expect(ring.read(b, none, rbuf.data(), rbuf.size(),
                   std::chrono::milliseconds(0))
             == 0,
         "late joiner sees no pre-attach backlog");

  // Three more frames are visible to both, each from its own cursor.
  for (std::uint64_t idx = 3; idx < 6; ++idx) {
    expect(ring.write(make_meta(idx), wbuf.data(), wbuf.size()), "write");
  }
  for (const int con : {a, b}) {
    for (std::uint64_t idx = 3; idx < 6; ++idx) {
      frame_ring::frame_meta got;
      const std::size_t size = ring.read(
          con, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
      expect(size == k_frame_bytes, "post-attach frame delivered");
      expect(got.pts == idx * 1000, "cursor unaffected by the other consumer");
    }
  }
  expect(ring.dropped_frames(a) == 0 && ring.dropped_frames(b) == 0,
         "neither consumer dropped");
  std::puts("ok: cursor independence");
}

// ---------------------------------------------------------------------------
// producer never blocks: writer completes a fixed volume while a consumer
// stalls holding nothing; wall time bounded (no consumer-paced waits).
// ---------------------------------------------------------------------------
auto test_producer_never_blocks() -> void
{
  frame_ring ring(1);
  const int con = ring.add_consumer();
  (void)con;  // never read from: permanently stalled consumer

  std::vector<std::uint8_t> wbuf(k_frame_bytes, 0xAB);

  const auto begin = std::chrono::steady_clock::now();
  const std::uint64_t total_frames = 300;  // ~90 MB: ~5.5 laps of a stalled peer
  for (std::uint64_t idx = 0; idx < total_frames; ++idx) {
    expect(ring.write(make_meta(idx), wbuf.data(), wbuf.size()), "write");
  }
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  expect(elapsed < std::chrono::seconds(5), "producer completed unhindered");
  std::puts("ok: producer never blocks");
}

// ---------------------------------------------------------------------------
// oversized / empty frames are dropped whole, ring untouched
// ---------------------------------------------------------------------------
auto test_drop_whole() -> void
{
  frame_ring ring(1);
  const int con = ring.add_consumer();
  std::vector<std::uint8_t> wbuf(k_frame_bytes, 0x33);

  expect(!ring.write(make_meta(0), wbuf.data(), 0), "empty frame rejected");
  expect(!ring.write(make_meta(0), nullptr, 10), "null payload rejected");
  expect(!ring.write(make_meta(0), wbuf.data(), ring.capacity() + 1),
         "oversized frame rejected");
  expect(ring.head() == 0, "rejected frames do not advance the head");

  expect(ring.write(make_meta(1), wbuf.data(), wbuf.size()), "valid write");
  std::vector<std::uint8_t> rbuf(ring.capacity());
  frame_ring::frame_meta got;
  const std::size_t size =
      ring.read(con, got, rbuf.data(), rbuf.size(), std::chrono::milliseconds(0));
  expect(size == k_frame_bytes, "following valid frame delivered");
  std::puts("ok: drop whole");
}

// ---------------------------------------------------------------------------
// close() wakes a blocked consumer
// ---------------------------------------------------------------------------
auto test_close_wakes() -> void
{
  frame_ring ring(1);
  const int con = ring.add_consumer();
  std::vector<std::uint8_t> rbuf(1024);

  std::thread closer(
      [&ring]() -> void
      {
        std::this_thread::sleep_for(k_tick);
        ring.close();
      });
  frame_ring::frame_meta got;
  const auto begin = std::chrono::steady_clock::now();
  const std::size_t size =
      ring.read(con, got, rbuf.data(), rbuf.size(), std::chrono::seconds(30));
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  closer.join();
  expect(size == 0, "close returns no frame");
  expect(elapsed < std::chrono::seconds(10), "close woke the reader early");
  std::puts("ok: close wakes");
}
}  // namespace

auto main() -> int
{
  test_construction();
  test_wrap();
  test_drop_oldest_whole_frames();
  test_slot_lap();
  test_cursor_independence();
  test_producer_never_blocks();
  test_drop_whole();
  test_close_wakes();
  std::puts("frame_ring_test: ALL OK");
  return 0;
}
