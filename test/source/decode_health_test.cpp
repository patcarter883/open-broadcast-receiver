// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// decode_health_test — the decision to retire a decode attempt for producing no
// frames, exercised against the numbers the decision is made from.
//
// The regression this test pins: a window armed at the demuxer's first video
// buffer that expires on wall-clock alone retires a healthy hardware decoder on
// a slowly-arriving ingest. The reported case is 185 KiB of a 6.3 MB fixture fed
// by the three-second mark: the old rule (window elapsed AND nothing decoded)
// fired there and rebuilt the stage with the software decoder, even though
// vaav1dec decodes the same bytes. The data-armed rule requires a real slice of
// the stream first, so that case cannot fire, while a decoder that is genuinely
// silent with plenty of data still fails inside the base window.
//
// Framework-free; exit 0 = pass. No GStreamer.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "fanout/decode_health.h"

namespace
{
using std::chrono::milliseconds;

auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

constexpr std::uint64_t k_kib = 1024U;
constexpr std::uint64_t k_mib = 1024U * 1024U;

auto health(std::uint64_t fed, std::uint64_t video, std::uint64_t decoded,
            milliseconds elapsed) -> decode_health::attempt_health
{
  return decode_health::attempt_health {
      .fed_bytes = fed,
      .video_buffers = video,
      .decoded_buffers = decoded,
      .elapsed = elapsed,
  };
}
}  // namespace

auto main() -> int
{
  using decode_health::no_frame_failure;
  using decode_health::window_for;

  // ---- the reported false fallback cannot fire ---------------------------
  // 185 KiB fed, six video buffers out of tsdemux, nothing decoded yet, six
  // seconds into the attempt. Wall-clock alone says "dead"; the volume says the
  // stream has barely started.
  expect(!no_frame_failure(health(185 * k_kib, 6, 0, milliseconds {6000})),
         "185 KiB fed retires the decoder (the premature-fallback regression)");
  // The same volume is still not evidence however long it takes to arrive.
  expect(!no_frame_failure(health(185 * k_kib, 60, 0, milliseconds {600000})),
         "a long wait does not make 185 KiB evidence of a broken decoder");

  // ---- below the minimum volume, no amount of time retires it -----------
  expect(!no_frame_failure(health(64 * k_kib, 12, 0, milliseconds {300000})),
         "64 KiB fed never authorises a fallback");
  expect(!no_frame_failure(
             health(decode_health::k_min_fed_bytes, 3, 0, milliseconds {300000})),
         "three video buffers are not enough to conclude the decoder is dead");

  // ---- genuine silence with real data still falls back -------------------
  // A wedged hardware decoder (pipeline parked in ASYNC, no bus error) takes
  // every buffer and emits nothing; with the minimum volume in, the base window
  // expires.
  expect(no_frame_failure(health(decode_health::k_min_fed_bytes, 4, 0,
                                 milliseconds {5000})),
         "a silent decoder with the minimum volume is retired at the base window");
  expect(!no_frame_failure(health(decode_health::k_min_fed_bytes, 4, 0,
                                  milliseconds {4999})),
         "the silent decoder is kept until its window has actually elapsed");
  // 2 MiB earns a longer window, and it still expires.
  expect(window_for(2 * k_mib) == milliseconds {7000},
         "2 MiB fed earns a 7 s window");
  expect(no_frame_failure(health(2 * k_mib, 40, 0, milliseconds {7000})),
         "a silent decoder with 2 MiB fed is retired when its window expires");
  expect(!no_frame_failure(health(2 * k_mib, 40, 0, milliseconds {6999})),
         "2 MiB of data holds the decoder for the whole 7 s");

  // ---- the window grows with what has arrived ---------------------------
  expect(window_for(0) == milliseconds {5000}, "an unfed attempt earns 5 s");
  expect(window_for(185 * k_kib) == milliseconds {5000},
         "185 KiB still earns the base window");
  expect(window_for(decode_health::k_min_fed_bytes) == milliseconds {5000},
         "the minimum volume earns the base window");
  expect(window_for(decode_health::k_min_fed_bytes + 1 * k_mib)
             == milliseconds {7000},
         "one MiB beyond the minimum buys two more seconds");
  expect(window_for(decode_health::k_min_fed_bytes + 3 * k_mib)
             == milliseconds {11000},
         "three MiB beyond the minimum buy six more seconds");
  expect(window_for(64 * k_mib) == decode_health::k_max_window,
         "the window is capped, so a fallback stays bounded");
  expect(window_for(512 * k_mib) == decode_health::k_max_window,
         "a huge volume does not grow the window past the cap");

  // The boundary of the scaled window is the scaled value, not a constant.
  {
    const std::uint64_t fed = decode_health::k_min_fed_bytes + 2 * k_mib;
    const auto window = window_for(fed);
    expect(window == milliseconds {9000}, "two MiB beyond the minimum earns 9 s");
    expect(!no_frame_failure(health(fed, 40, 0, window - milliseconds {1})),
           "the scaled window is honoured to the millisecond");
    expect(no_frame_failure(health(fed, 40, 0, window)),
           "the scaled window expires when it is reached");
  }

  // ---- an attempt that produced a frame is never retired ----------------
  expect(!no_frame_failure(health(64 * k_mib, 4000, 1, milliseconds {600000})),
         "an attempt that decoded anything is not retired for silence");

  // ---- the demux verdict is data-armed too ------------------------------
  using decode_health::demux_failure;
  // 52 KiB fed, no video pad, ten seconds in: that is a stream that is still
  // arriving, not a TS without video. This is the shape of the measured failure
  // on a 10 KB/s local feed under the old flat window.
  expect(!demux_failure(52 * k_kib, 0, milliseconds {10000}),
         "52 KiB fed does not authorise a demux verdict");
  expect(!demux_failure(decode_health::k_min_fed_bytes, 0, milliseconds {4999}),
         "the demux verdict waits for its window");
  expect(demux_failure(decode_health::k_min_fed_bytes, 0, milliseconds {5000}),
         "the minimum volume with no pad reaches a demux verdict");
  expect(!demux_failure(decode_health::k_min_fed_bytes, 3, milliseconds {600000}),
         "a demux verdict needs no video buffer at all");
  expect(!demux_failure(2 * k_mib, 0, milliseconds {7000 - 1}),
         "the demux window scales with the volume as well");
  expect(demux_failure(2 * k_mib, 0, milliseconds {7000}),
         "the scaled demux window expires when it is reached");

  std::printf("decode_health_test: ok\n");
  return 0;
}
