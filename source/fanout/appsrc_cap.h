// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#pragma once

#include <algorithm>
#include <cstdint>

// Sizing for the transcode video appsrc.
//
// The queue is capped in FRAMES, not bytes: a fixed byte cap is a different
// amount of slack at every resolution (4 MB is 2.9 frames of 720p NV12 and 7.7
// of 360p), so one number cannot mean "a few frames ahead of the encoder" for
// both. The frame size comes from the frame ring's metadata once the first
// decoded frame arrives, and the cap is restated from it then.
//
// The floor keeps small frames from being capped tighter than the element's
// own default; the ceiling bounds what a large frame may reserve, and a stalled
// consumer is handled by the ring's drop-oldest behaviour rather than by
// growing this queue.

namespace appsrc_cap
{

// Eight frames hold ~0.27 s of 720p, enough for the feed to stay ahead of an
// encoder running at ingest rate without adding meaningful latency.
inline constexpr std::uint64_t k_frame_span = 8;
inline constexpr std::uint64_t k_floor_bytes = 4ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_ceiling_bytes = 64ull * 1024ull * 1024ull;

// Bytes in one NV12 frame of `width` x `height` luma samples: two planes, the
// chroma plane being a quarter of the luma one.
inline auto nv12_frame_bytes(std::uint64_t width, std::uint64_t height)
    -> std::uint64_t
{
  return width * height * 3ull / 2ull;
}

// max-bytes for a queue holding k_frame_span frames of `frame_bytes`.
inline auto max_bytes(std::uint64_t frame_bytes) -> std::uint64_t
{
  return std::clamp(frame_bytes * k_frame_span, k_floor_bytes, k_ceiling_bytes);
}

// Whole frames the resulting cap holds. A cap at k_ceiling_bytes holds fewer
// frames than k_frame_span for a large frame, which is the intended bound.
inline auto frames_at(std::uint64_t cap, std::uint64_t frame_bytes)
    -> std::uint64_t
{
  return frame_bytes == 0 ? 0 : cap / frame_bytes;
}

}  // namespace appsrc_cap
