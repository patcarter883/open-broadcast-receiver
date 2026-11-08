// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the transcode appsrc cap: the queue depth is a frame count of
// the negotiated frame, so the byte cap has to grow with the resolution and
// stay bounded at the top end, and a small source must not end up with less
// slack than the element's default. Framework-free; exit 0 = pass. No
// GStreamer, so the fast (tests-only) lane runs it.

#include <cstdio>
#include <cstdlib>

#include "fanout/appsrc_cap.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto expect_eq(std::uint64_t got, std::uint64_t want, const char* what) -> void
{
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s (got %llu, want %llu)\n", what,
                 static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(want));
    std::exit(1);
  }
}
}  // namespace

auto main() -> int
{
  using namespace appsrc_cap;

  // 720p and 1080p are the ingest sizes this deployment runs; a cap that holds
  // fewer than k_frame_span frames there is the regression: the feed then
  // stalls against the queue instead of riding out a scheduling gap.
  const std::uint64_t f720 = nv12_frame_bytes(1280, 720);
  const std::uint64_t f1080 = nv12_frame_bytes(1920, 1080);
  const std::uint64_t f2160 = nv12_frame_bytes(3840, 2160);

  expect_eq(f720, 1280ull * 720ull * 3ull / 2ull, "NV12 720p frame size");
  expect_eq(f1080, 1920ull * 1080ull * 3ull / 2ull, "NV12 1080p frame size");
  expect_eq(f720, 1382400ull, "NV12 720p frame is 1.32 MiB");

  const std::uint64_t cap720 = max_bytes(f720);
  const std::uint64_t cap1080 = max_bytes(f1080);
  expect(frames_at(cap720, f720) >= k_frame_span,
         "the cap holds at least k_frame_span frames of 720p");
  expect(frames_at(cap1080, f1080) >= k_frame_span,
         "the cap holds at least k_frame_span frames of 1080p");
  expect_eq(cap1080, f1080 * k_frame_span,
            "1080p is unbounded by the ceiling, so it is an exact frame count");

  // The byte cap is derived from the frame, so it scales with the resolution
  // rather than staying a constant.
  expect(cap1080 > cap720, "1080p gets a larger cap than 720p");
  expect(cap720 > k_floor_bytes, "720p is above the floor, so it is a frame count");

  // 4K is where the ceiling takes over: the cap stops following the frame
  // count and holds what it can, because dropping transcode frames is
  // preferable to reserving 100+ MB for a delayed encoder.
  const std::uint64_t cap2160 = max_bytes(f2160);
  expect_eq(cap2160, k_ceiling_bytes, "4K is capped at the ceiling");
  expect(cap2160 > cap1080, "4K still gets more room than 1080p");
  expect(frames_at(cap2160, f2160) >= 3,
         "the 4K cap still holds several frames");
  expect(frames_at(cap2160, f2160) < k_frame_span,
         "the 4K cap is below the frame span, which is what the ceiling means");

  // A tiny source keeps the element default rather than a two-frame queue.
  expect_eq(max_bytes(nv12_frame_bytes(320, 180)), k_floor_bytes,
            "a small frame is floored at the element default");
  expect_eq(frames_at(k_floor_bytes, nv12_frame_bytes(320, 180)) >= 20, true,
            "the floor still holds tens of small frames");

  std::printf("appsrc_cap_test: all assertions passed\n");
  return 0;
}
