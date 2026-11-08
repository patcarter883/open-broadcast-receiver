// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_TIMELINE_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_TIMELINE_H

#include <cstdint>

// timeline_state — the re-stamper: it puts every buffer of a session's fan-out
// on ONE monotonic timeline, whatever the upstream source does.
//
// Why this is the correctness centre of the standby feature: a live encoder
// that stops, restarts, reconnects inside the hysteresis window, or resumes with
// its own clock after a stall delivers timestamps that jump backwards or jump
// forward by minutes. Passed through, those are a stalled player, a muxer queue
// that never drains and a destination that reconnects. Rewritten to a
// continuous timeline, the fan-out never notices: the muxers keep publishing,
// the destinations stay connected, and the only visible artefact is one frame
// of hold.
//
// Contract:
//  - PTS and DTS are rewritten by the SAME offset. A B-frame's pts-dts delta is
//    its reorder distance; moving only the PTS would make the muxer reorder or
//    stall. The delta in, the delta out.
//  - A backwards jump (a stamp at or before last_out) is clamped to
//    last_out + frame_duration, and the offset is shifted by the difference so
//    the buffers after it line up again.
//  - A forward jump larger than max_gap_ns is compressed to
//    last_out + frame_duration, likewise absorbing the difference into the
//    offset.
//  - Any other forward step passes through with the current offset.
//  - ONE INSTANCE IS SHARED BY THE VIDEO AND AUDIO STREAMS OF A SESSION. That
//    is the whole point: two independent re-stampers would give the streams
//    different offsets and the lip-sync would drift a little at every switch.
//    Both streams take the state's offset, and anchor() is called once at a
//    source switch so the incoming source's first stamp lands one frame after
//    the last published one — for both streams at once.
//  - Consequence of sharing: apply() advances last_out, the single published
//    floor. The stream whose timestamps drive the timeline calls apply(); the
//    other stream's buffers are rewritten with offset_ns() (they must not move
//    the floor, or an audio buffer a few milliseconds behind the video would
//    clamp and lift the offset under the video). Both are still shifted by the
//    same offset, which is what A/V sync needs.
//
// Pure arithmetic and state: no GStreamer types, so it is unit-testable and the
// pad-probe that calls it (a later task) stays a two-line adapter.
class timeline_state
{
public:
  // frame_duration_ns is the nominal output frame duration (one frame at the
  // negotiated rate); it is the minimum step the timeline ever takes and is
  // clamped up to 1 ns. max_gap_ns is the largest forward step passed through
  // unchanged (the config's standby.max_gap_ms); it is clamped up to at least
  // one frame.
  timeline_state(std::int64_t frame_duration_ns, std::int64_t max_gap_ns);

  // Anchor the incoming source so its first stamp lands one frame after the
  // last published one. Call at every source switch, before the first buffer of
  // the new source is applied. On a fresh state (nothing published yet) the
  // offset stays 0: the timeline starts on the source's own clock.
  auto anchor(std::int64_t first_in_pts_ns) -> void;

  // Rewrite one buffer's stamps in place. Returns false if the buffer must be
  // dropped: a malformed stamp (dts after pts, or a rewrite that cannot be made
  // strictly monotonic). A refused buffer changes no state.
  auto apply(std::int64_t& pts_ns, std::int64_t& dts_ns) -> bool;

  // The last published output PTS (0 before anything is published).
  [[nodiscard]] auto last_out_ns() const noexcept -> std::int64_t
  {
    return m_last_out;
  }

  // The single offset both streams are rewritten with.
  [[nodiscard]] auto offset_ns() const noexcept -> std::int64_t
  {
    return m_offset;
  }

  [[nodiscard]] auto frame_duration_ns() const noexcept -> std::int64_t
  {
    return m_duration;
  }

  [[nodiscard]] auto published() const noexcept -> bool
  {
    return m_have_last;
  }

private:
  std::int64_t m_duration;
  std::int64_t m_max_gap;
  std::int64_t m_offset = 0;
  std::int64_t m_last_out = 0;
  bool m_have_last = false;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_TIMELINE_H
