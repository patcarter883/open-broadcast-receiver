// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_PTS_SYNTH_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_PTS_SYNTH_H

#include <cstdint>

// pts_synth — turns the decoded frame stream into a timeline a live muxer can
// publish.
//
// Decoders do not guarantee a timestamp on every output frame: vaav1dec emits
// buffers whose PTS is absent for a frame whose PES carried none, and it emits
// no DTS at all. Forwarding those as 0 puts a frame at the start of the
// timeline in the middle of the stream; flvmux then reorders and rtmp2sink
// holds the frame for its (bogus) earlier presentation time, so the publish
// stalls and resumes in bursts and the output's appsrc fills.
//
// The rule here is the one a live destination needs: the timeline is monotonic.
// A frame without a PTS, or one that does not advance the timeline, is placed
// one nominal frame duration after the last accepted frame. DTS is the PTS:
// decoded output is in display order and the encoder assigns its own DTS.
class pts_synth
{
public:
  // Nominal frame duration from the decoded caps' frame rate; 0 when the caps
  // carry no usable rate (a missing timestamp then advances by the smallest
  // step).
  auto set_frame_duration(std::int64_t duration_ns) noexcept -> void
  {
    m_duration = duration_ns > 0 ? duration_ns : 0;
  }

  struct stamp {
    std::int64_t pts = 0;
    std::int64_t dts = 0;
  };

  // Accepts the frame's own PTS when it is present and advances the timeline;
  // otherwise synthesises the next slot.
  auto next(bool pts_valid, std::int64_t pts_ns) noexcept -> stamp
  {
    const std::int64_t step = m_duration > 0 ? m_duration : 1;
    std::int64_t pts = pts_ns;
    if (!pts_valid) {
      pts = m_have_last ? m_last + step : 0;
    } else if (m_have_last && pts <= m_last) {
      pts = m_last + step;
    }
    m_last = pts;
    m_have_last = true;
    return {.pts = pts, .dts = pts};
  }

  auto reset() noexcept -> void
  {
    m_last = 0;
    m_have_last = false;
  }

private:
  std::int64_t m_duration = 0;
  std::int64_t m_last = 0;
  bool m_have_last = false;
};

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_PTS_SYNTH_H
