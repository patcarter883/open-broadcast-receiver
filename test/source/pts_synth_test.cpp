// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for pts_synth — the monotonic timeline the decode stage hands to
// every transcode output. Dependency-free (no test framework, no GStreamer):
// each check aborts with a message on failure; exit 0 means pass.
//
// The case that matters is the one measured on the rig: a decoded frame with no
// PTS used to be published as 0, which put a frame at the head of the timeline
// in the middle of the stream and made rtmp2sink hold the following frames.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "fanout/pts_synth.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

constexpr std::int64_t k_frame_30fps = 33333333;  // ns
}  // namespace

auto main() -> int
{
  // Every frame carries a PTS and advances: passed through unchanged.
  {
    pts_synth synth;
    synth.set_frame_duration(k_frame_30fps);
    const auto first = synth.next(true, 5000000000);
    const auto second = synth.next(true, 5033333333);
    expect(first.pts == 5000000000, "first PTS passed through");
    expect(second.pts == 5033333333, "advancing PTS passed through");
  }

  // A frame with no PTS lands one frame duration after the last accepted frame
  // instead of at 0 (this is the publish stall).
  {
    pts_synth synth;
    synth.set_frame_duration(k_frame_30fps);
    synth.next(true, 5000000000);
    const auto missing = synth.next(false, 0);
    expect(missing.pts == 5000000000 + k_frame_30fps,
           "missing PTS synthesised one frame after the last");
    expect(missing.pts != 0, "missing PTS never rewinds the timeline");
    const auto next = synth.next(true, 5000000000 + 2 * k_frame_30fps);
    expect(next.pts == 5000000000 + 2 * k_frame_30fps,
           "the real timeline resumes after a synthesised frame");
  }

  // A repeated or backwards PTS is moved forward, never emitted twice.
  {
    pts_synth synth;
    synth.set_frame_duration(k_frame_30fps);
    synth.next(true, 1000000);
    const auto duplicate = synth.next(true, 1000000);
    expect(duplicate.pts == 1000000 + k_frame_30fps, "duplicate PTS advanced");
    const auto backwards = synth.next(true, 999999);
    expect(backwards.pts == 1000000 + 2 * k_frame_30fps,
           "backwards PTS advanced");
  }

  // The first frame has no predecessor: an invalid PTS starts the timeline at 0.
  {
    pts_synth synth;
    synth.set_frame_duration(k_frame_30fps);
    const auto first = synth.next(false, 0);
    expect(first.pts == 0, "first synthesised PTS starts at 0");
  }

  // No frame rate in the caps: the timeline still advances.
  {
    pts_synth synth;
    synth.set_frame_duration(0);
    synth.next(true, 1000);
    const auto missing = synth.next(false, 0);
    expect(missing.pts > 1000, "unknown frame rate still advances");
  }

  // DTS follows the PTS: decoded output is in display order.
  {
    pts_synth synth;
    synth.set_frame_duration(k_frame_30fps);
    const auto s = synth.next(true, 42);
    expect(s.dts == s.pts, "DTS equals PTS");
  }

  std::printf("pts_synth: all checks passed\n");
  return 0;
}
