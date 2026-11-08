// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODE_HEALTH_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODE_HEALTH_H

#include <algorithm>
#include <chrono>
#include <cstdint>

// decode_health — the decision to retire a decode attempt, expressed in data
// rather than in wall-clock alone.
//
// A window that starts counting the moment the demuxer hands over its first
// video buffer says nothing about how much of the stream the decoder has
// actually been given. On a trickling ingest that window can expire with a few
// hundred kilobytes fed, which is not evidence of a broken decoder: the same
// element decodes the same bytes happily once they arrive. So an attempt is
// only retired for a silent decoder once it has been given a meaningful slice
// of the stream (a minimum number of fed bytes AND of demuxed video buffers),
// and the window itself grows with the volume that has arrived, so a slow
// start stretches the deadline instead of triggering it.
//
// The demux window keeps the same shape: it is armed by the attempt's first TS
// chunk, floored by the same minimum volume, and scaled by the bytes fed since,
// so a stream that is arriving slowly is not declared pad-less on the strength
// of a few tens of kilobytes and five seconds.
//
// Everything here is pure arithmetic: the caller supplies the counters it has
// already collected, and the same numbers are what the periodic log line
// prints. That keeps the judgement unit-testable without GStreamer.
namespace decode_health
{

// Below this many TS bytes fed to the attempt there is no evidence about the
// decoder at all: a keyframe plus its sequence header at product bitrates is
// comfortably under it, and a stream that has delivered less has simply not
// started.
inline constexpr std::uint64_t k_min_fed_bytes = 256U * 1024U;

// And below this many video buffers out of the demuxer the decoder may still be
// waiting for the access unit that carries its first frame. A working decoder
// emits within one or two buffers; four is the point at which silence is the
// decoder's, not the stream's.
inline constexpr std::uint64_t k_min_video_buffers = 4U;

// The window granted on top of the data minimums, and how far it may grow: each
// whole MiB fed beyond the minimum buys k_scale_step more, up to k_max_window.
// A decoder that cannot decode at all still fails inside the base window once
// the minimum data is in, so the fallback stays bounded.
inline constexpr std::chrono::seconds k_base_window {5};
inline constexpr std::chrono::seconds k_scale_step {2};
inline constexpr std::chrono::seconds k_max_window {25};

// What an attempt has been given and produced since it was built.
struct attempt_health {
  std::uint64_t fed_bytes = 0;       // TS bytes pushed into the decode appsrc
  std::uint64_t video_buffers = 0;   // video buffers out of tsdemux
  std::uint64_t decoded_buffers = 0; // buffers out of the decoder
  std::chrono::milliseconds elapsed {0};  // since the attempt's first video buffer
};

// The deadline granted to an attempt that has been fed `fed_bytes`: the base
// window, plus one step per MiB past the minimum, capped.
[[nodiscard]] inline auto window_for(std::uint64_t fed_bytes)
    -> std::chrono::milliseconds
{
  constexpr std::uint64_t k_max_steps =
      static_cast<std::uint64_t>((k_max_window - k_base_window) / k_scale_step);
  const std::uint64_t steps = fed_bytes > k_min_fed_bytes
      ? std::min<std::uint64_t>((fed_bytes - k_min_fed_bytes) / (1024U * 1024U),
                                k_max_steps)
      : 0U;
  const auto window =
      k_base_window + k_scale_step * static_cast<std::int64_t>(steps);
  return std::chrono::duration_cast<std::chrono::milliseconds>(window);
}

// True when a silent decoder is evidence of a broken decoder: the attempt has
// been given the minimum slice of the stream and has produced nothing inside
// the window that slice earns.
[[nodiscard]] inline auto no_frame_failure(const attempt_health& h) -> bool
{
  return h.decoded_buffers == 0
      && h.fed_bytes >= k_min_fed_bytes
      && h.video_buffers >= k_min_video_buffers
      && h.elapsed >= window_for(h.fed_bytes);
}

// The step before it: tsdemux has been fed and has produced no video buffer at
// all. A private AV1 stream (stream_type 0x06 + AV1G) is only mapped to a
// video pad once the demuxer has sniffed enough of it, so this one is armed and
// floored the same way -- a slow start must not look like a TS without video.
[[nodiscard]] inline auto demux_failure(std::uint64_t fed_bytes,
                                        std::uint64_t video_buffers,
                                        std::chrono::milliseconds elapsed) -> bool
{
  return video_buffers == 0 && fed_bytes >= k_min_fed_bytes
      && elapsed >= window_for(fed_bytes);
}

}  // namespace decode_health

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_DECODE_HEALTH_H
