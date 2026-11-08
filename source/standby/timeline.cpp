// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "standby/timeline.h"

#include <limits>

namespace
{
// Checked arithmetic: the re-stamper must never wrap a timestamp (a wrapped
// int64 is a wild jump in the published timeline, not an error any muxer can
// recover from). Returns false on overflow.
auto checked_add(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) noexcept
    -> bool
{
  if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
    return false;
  }
  if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

// rhs - lhs, false on overflow.
auto checked_sub(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) noexcept
    -> bool
{
  if (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs) {
    return false;
  }
  if (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) {
    return false;
  }
  out = lhs - rhs;
  return true;
}
}  // namespace

timeline_state::timeline_state(std::int64_t frame_duration_ns,
                               std::int64_t max_gap_ns)
    : m_duration {frame_duration_ns > 0 ? frame_duration_ns : 1}
    // A max_gap below one frame would make every normal step look like a
    // discontinuity, so it can never be tighter than the minimum step.
    , m_max_gap {max_gap_ns > m_duration ? max_gap_ns : m_duration}
{
}

auto timeline_state::anchor(std::int64_t first_in_pts_ns) -> void
{
  // Nothing published yet: the timeline starts on the source's own clock, so
  // there is nothing to align to.
  if (!m_have_last) {
    m_offset = 0;
    return;
  }
  std::int64_t target = 0;
  if (!checked_add(m_last_out, m_duration, target)) {
    return;  // absurd last_out; leave the offset as it is
  }
  std::int64_t offset = 0;
  if (!checked_sub(target, first_in_pts_ns, offset)) {
    return;
  }
  m_offset = offset;
}

auto timeline_state::apply_driver(std::int64_t& pts_ns, std::int64_t& dts_ns)
    -> bool
{
  // A buffer whose DTS is after its PTS is not a reorder delay, it is a broken
  // stamp; no offset makes it legal.
  if (dts_ns > pts_ns) {
    return false;
  }
  const std::int64_t reorder = pts_ns - dts_ns;

  std::int64_t out = 0;
  if (!checked_add(pts_ns, m_offset, out)) {
    return false;
  }

  // Candidate state, committed only once every check has passed.
  std::int64_t offset = m_offset;
  std::int64_t last_out = out;

  if (m_have_last) {
    const std::int64_t step = out - m_last_out;  // no overflow: values adjacent
    // A backwards (or duplicate) stamp, or a forward hole longer than max_gap,
    // is placed exactly one frame after the last published stamp; the offset
    // absorbs the difference so the following buffers line up again.
    if (step <= 0 || step > m_max_gap) {
      std::int64_t target = 0;
      if (!checked_add(m_last_out, m_duration, target)) {
        return false;
      }
      std::int64_t shift = 0;
      if (!checked_sub(target, out, shift)) {
        return false;
      }
      std::int64_t shifted = 0;
      if (!checked_add(m_offset, shift, shifted)) {
        return false;
      }
      offset = shifted;
      last_out = target;
    }
    // Defensive: a clamp that did not strictly advance the timeline would let a
    // duplicate through, so refuse it (state still unchanged).
    if (last_out <= m_last_out) {
      return false;
    }
  }

  // PTS and DTS take the same offset, so their delta (the reorder distance) is
  // exactly what it was. Derived from the delta so the two writes cannot
  // disagree.
  std::int64_t dts_out = 0;
  if (!checked_sub(last_out, reorder, dts_out)) {
    return false;
  }

  m_offset = offset;
  m_last_out = last_out;
  m_have_last = true;
  pts_ns = last_out;
  dts_ns = dts_out;
  return true;
}

auto timeline_state::apply_follower(std::int64_t& pts_ns, std::int64_t& dts_ns)
    -> bool
{
  // Same legality test as the driver: a reorder distance is a pts-dts delta,
  // never a negative one.
  if (dts_ns > pts_ns) {
    return false;
  }
  const std::int64_t reorder = pts_ns - dts_ns;

  std::int64_t out = 0;
  if (!checked_add(pts_ns, m_offset, out)) {
    return false;
  }

  // The follower takes exactly the offset the driver chose, and then checks its
  // OWN floor: the last stamp it emitted. A source switch can move the shared
  // offset backwards (a forward-gap compression, or a re-anchor onto a source
  // whose clock is behind), which would make an audio buffer already stamped on
  // the old clock land before audio it has already published -- a muxer error,
  // not a tolerance. Bump it one nanosecond past the follower's last output.
  // The cost is at most one audio frame of A/V skew at a switch (inaudible);
  // the alternative is a backwards DTS the muxer cannot encode.
  //
  // This is the ONLY thing the follower clamps: it never touches m_last_out or
  // m_offset. Those belong to the driver, and a late audio buffer must not be
  // able to lift the video's floor out from under it.
  if (m_have_follower && out <= m_last_follower) {
    std::int64_t bumped = 0;
    if (!checked_add(m_last_follower, 1, bumped)) {
      return false;
    }
    out = bumped;
  }

  std::int64_t dts_out = 0;
  if (!checked_sub(out, reorder, dts_out)) {
    return false;
  }

  m_last_follower = out;
  m_have_follower = true;
  pts_ns = out;
  dts_ns = dts_out;
  return true;
}
