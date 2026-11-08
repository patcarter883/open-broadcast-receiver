// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the link-derived transcode rate (DECISIONS §20): the encoder
// target follows the measured ingest rate with a quality floor, an absolute
// ceiling and re-target hysteresis, and the portal's `bitrate_kbps` is only the
// rate until a measurement exists. The connect bootstrap is the rate an attempt
// opens at before publish is confirmed, and is capped below that target so the
// transit buffer outlasts the RTMP handshake. Framework-free; exit 0 = pass. No
// GStreamer.

#include <cstdio>
#include <cstdlib>

#include "lib/lib.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto expect_eq(int got, int want, const char* what) -> void
{
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
    std::exit(1);
  }
}
}  // namespace

auto main() -> int
{
  using policy = transcode_bitrate_policy;

  // No measurement yet (session just started): the portal's figure is what the
  // encoder is asked for, exactly as before this policy existed.
  expect_eq(derive_transcode_bitrate_kbps(0, 29860), 29860,
            "no measurement falls back to the portal figure");
  expect_eq(derive_transcode_bitrate_kbps(0, 0), 0,
            "no measurement and no portal figure leaves the encoder default");
  expect_eq(derive_transcode_bitrate_kbps(-1, 8000), 8000,
            "a negative measurement is not a measurement");

  // The derived target MOVES with the measured link: this is the property the
  // portal constant could not have.
  expect_eq(derive_transcode_bitrate_kbps(20000, 29860), 18000,
            "20 Mbit link derives 90% of it, not the portal's 29860");
  expect_eq(derive_transcode_bitrate_kbps(10000, 29860), 9000,
            "10 Mbit link derives 9000");
  expect_eq(derive_transcode_bitrate_kbps(4000, 29860), 3600,
            "4 Mbit link derives 3600");
  expect(derive_transcode_bitrate_kbps(20000, 29860)
             < derive_transcode_bitrate_kbps(22000, 29860),
         "the target is monotonic in the measured rate");

  // The portal value is a fallback, not a cap: a link measured above it does
  // not get pulled down to the destination's metadata figure.
  expect_eq(derive_transcode_bitrate_kbps(20000, 6000), 18000,
            "a low portal figure does not cap a measured link");
  // ... and a portal figure above the link does not survive the measurement.
  expect(derive_transcode_bitrate_kbps(20000, 29860) < 29860,
         "the portal's 29860 does not survive a 20 Mbit measurement");

  // Ceiling: whatever the measurement says, one live re-encode is not asked for
  // more than the policy allows.
  expect_eq(derive_transcode_bitrate_kbps(60000, 29860),
            policy::ceiling_kbps,
            "60 Mbit link is clamped to the ceiling");
  expect_eq(derive_transcode_bitrate_kbps(30000, 0), policy::ceiling_kbps,
            "ceiling applies without a portal figure too");

  // Floor: a link measured above the floor never gets less than the floor ...
  expect_eq(derive_transcode_bitrate_kbps(1200, 29860), 1080,
            "just above floor*10/9 derives above the floor");
  // ... but a link BELOW the floor wins over it: asking for more than the link
  // carries is the failure this policy exists to prevent.
  expect_eq(derive_transcode_bitrate_kbps(1000, 29860), 900,
            "a link under the floor derives under the floor");
  expect_eq(derive_transcode_bitrate_kbps(500, 29860), 450,
            "a 500 kbps link derives 450, never the floor");

  // Hysteresis: per-second jitter must not re-target a running encoder.
  expect(!bitrate_change_is_material(18000, 18000), "an unchanged target is a no-op");
  expect(!bitrate_change_is_material(18000, 18200), "1.1% is not worth re-targeting");
  expect(!bitrate_change_is_material(18000, 17400), "3.3% is not worth re-targeting");
  expect(bitrate_change_is_material(18000, 16000), "11% is worth re-targeting");
  expect(!bitrate_change_is_material(18000, 19100),
         "6.1% is under the share threshold even past 500 kbps");
  expect(bitrate_change_is_material(18000, 22000), "22% is worth re-targeting");
  expect(bitrate_change_is_material(0, 5000), "entering a real target is material");
  expect(bitrate_change_is_material(5000, 0), "leaving for the default is material");

  // Connect bootstrap: an attempt opens at a rate the transit buffer can hold
  // through the RTMP handshake, and the measured-link target is what the encoder
  // is re-targeted to once publish is confirmed. A 5747 kbps measurement against
  // a declared 2000 kbps derives 5172 kbps for the steady target; the attempt
  // has to open at 2000 instead, not at that target.
  expect_eq(derive_transcode_bitrate_kbps(5747, 2000), 5172,
            "the steady target on a 5747 kbps measurement is still 5172");
  expect_eq(derive_transcode_bootstrap_kbps(5747, 2000), 2000,
            "the connect bootstrap is the declared 2000, not the 5172 target");
  expect(derive_transcode_bootstrap_kbps(5747, 0)
             == policy::bootstrap_ceiling_kbps,
         "with no declared rate the bootstrap is the ceiling, not the target");
  expect(derive_transcode_bootstrap_kbps(5747, 0)
             < derive_transcode_bitrate_kbps(5747, 0),
         "the bootstrap is strictly below the target it ramps to");
  expect_eq(derive_transcode_bootstrap_kbps(5747, 800), 800,
            "a declared rate below the ceiling is used as-is");
  expect(derive_transcode_bootstrap_kbps(1000, 2000)
             == derive_transcode_bitrate_kbps(1000, 2000),
         "a link measured below the bootstrap keeps its own lower target");
  // First attempt, no measurement yet: the bootstrap is the declared rate, which
  // is what a fresh session has always opened with.
  expect_eq(derive_transcode_bootstrap_kbps(0, 2000), 2000,
            "no measurement leaves the bootstrap at the declared rate");
  expect_eq(derive_transcode_bootstrap_kbps(0, 29860), 29860,
            "no measurement does not cap the declared rate");
  expect_eq(derive_transcode_bootstrap_kbps(0, 0), 0,
            "no measurement and no declared rate leaves the encoder default");

  std::printf("bitrate_test: all assertions passed\n");
  return 0;
}
