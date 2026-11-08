// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for timeline_state — the re-stamper that gives the whole fan-out
// one monotonic timeline. Dependency-free (no test framework, no GStreamer
// types): each check aborts with a message on failure; exit 0 means pass. Run
// via ctest.
//
// The properties under test, in the order they can break a show:
//  1. PTS and DTS move by the SAME offset, so a B-frame's pts-dts delta — the
//     decoder's reorder distance — survives every rewrite.
//  2. A backwards jump (encoder restart, stage rebuild, source switch back to a
//     queued buffer) clamps to last_out + one frame, and the buffers after it
//     line up again one frame apart.
//  3. A forward jump larger than max_gap_ns (an encoder resuming on its own
//     clock) compresses to one frame, never to a multi-second freeze.
//  4. Whatever the source does, the published timeline is strictly increasing.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "standby/timeline.h"

namespace
{
constexpr std::int64_t k_frame = 33'333'333;          // 30 fps, ns
constexpr std::int64_t k_max_gap = 1'000'000'000;     // 1000 ms
constexpr std::int64_t k_reorder = 2 * k_frame;       // B-frame pts-dts delta

auto expect(bool cond, const std::string& what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
  }
}

// One source buffer: PTS in display order, DTS two frames earlier (a B-frame
// pipeline's constant reorder distance).
struct stamp_pair
{
  std::int64_t pts = 0;
  std::int64_t dts = 0;
};

auto make_stamp(std::int64_t src_pts) -> stamp_pair
{
  return {.pts = src_pts, .dts = src_pts - k_reorder};
}

// Apply to the driving stream (video), asserting the re-stamp succeeded.
auto drive_ok(timeline_state& timeline, stamp_pair stamp) -> stamp_pair
{
  expect(timeline.apply_driver(stamp.pts, stamp.dts),
         "apply_driver accepted the buffer");
  return stamp;
}

// Apply to a following stream (audio), asserting the re-stamp succeeded.
auto follow_ok(timeline_state& timeline, stamp_pair stamp) -> stamp_pair
{
  expect(timeline.apply_follower(stamp.pts, stamp.dts),
         "apply_follower accepted the buffer");
  return stamp;
}

// ---------------------------------------------------------------------------
// the first buffer passes through: a fresh timeline starts where the source is
// ---------------------------------------------------------------------------
auto test_first_buffer_passes_through() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  expect(!timeline.published(), "nothing published yet");
  expect(timeline.offset_ns() == 0, "a fresh state has no offset");

  const stamp_pair out = drive_ok(timeline, make_stamp(5 * k_frame));
  expect(out.pts == 5 * k_frame, "first PTS passes through unchanged");
  expect(out.dts == 5 * k_frame - k_reorder, "first DTS passes through");
  expect(timeline.last_out_ns() == 5 * k_frame, "last_out tracks the output");
  expect(timeline.published(), "a buffer has now been published");
  std::puts("ok: first buffer passes through");
}

// ---------------------------------------------------------------------------
// a normal forward step passes through with the current offset
// ---------------------------------------------------------------------------
auto test_normal_step_passes_through() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  std::int64_t src = 1000 * k_frame;
  std::int64_t prev_out = 0;
  for (int idx = 0; idx < 200; ++idx) {
    const stamp_pair out = drive_ok(timeline, make_stamp(src));
    if (idx != 0) {
      expect(out.pts - prev_out == k_frame, "one frame apart at 30 fps");
    }
    prev_out = out.pts;
    expect(timeline.offset_ns() == 0, "no offset is invented for a clean stream");
    src += k_frame;
  }
  std::puts("ok: normal step passes through");
}

// ---------------------------------------------------------------------------
// PTS and DTS are shifted by the same offset: the reorder delta is preserved
// ---------------------------------------------------------------------------
auto test_bframe_delta_preserved() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  std::int64_t src = 7 * k_frame;

  for (int idx = 0; idx < 50; ++idx) {
    const stamp_pair out = drive_ok(timeline, make_stamp(src));
    expect(out.pts - out.dts == k_reorder,
           "pts-dts delta preserved across a normal run");
    src += k_frame;
  }

  // Now force every rewrite path in turn and check the delta again.
  // (a) backwards jump
  src -= 20 * k_frame;
  stamp_pair out = drive_ok(timeline, make_stamp(src));
  expect(out.pts - out.dts == k_reorder, "delta preserved across a clamp");
  // (b) huge forward jump
  src += 90'000'000'000LL;  // 90 s on the encoder's own clock
  out = drive_ok(timeline, make_stamp(src));
  expect(out.pts - out.dts == k_reorder, "delta preserved across a compression");
  // (c) steady again
  src += k_frame;
  out = drive_ok(timeline, make_stamp(src));
  expect(out.pts - out.dts == k_reorder, "delta preserved after a compression");

  // The offset moved the whole pair, not just the PTS.
  expect(out.pts - (src) == out.dts - (src - k_reorder),
         "PTS and DTS took the identical offset");
  std::puts("ok: B-frame delta preserved");
}

// ---------------------------------------------------------------------------
// a backwards jump clamps to last_out + one frame and the stream lines up again
// ---------------------------------------------------------------------------
auto test_backwards_jump_clamps() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  std::int64_t src = 0;
  std::int64_t last = 0;
  for (int idx = 0; idx < 10; ++idx) {
    last = drive_ok(timeline, make_stamp(src)).pts;
    src += k_frame;
  }
  const std::int64_t offset_before = timeline.offset_ns();

  // The source jumps back ten frames (an encoder restart from its own base).
  src -= 10 * k_frame;
  stamp_pair out = drive_ok(timeline, make_stamp(src));
  expect(out.pts == last + k_frame, "backwards jump clamps to last + one frame");
  expect(timeline.offset_ns() > offset_before, "the offset absorbed the jump");
  expect(timeline.last_out_ns() == out.pts, "last_out follows the clamp");

  // The next source buffer is exactly one frame later on the source clock, so
  // on the output clock it must be exactly one frame after the clamped one.
  src += k_frame;
  const std::int64_t clamped = out.pts;
  out = drive_ok(timeline, make_stamp(src));
  expect(out.pts == clamped + k_frame,
         "the following buffer is exactly one frame later");

  // A duplicate stamp (the same PTS twice) is also clamped, not dropped: one
  // frame of duplication is invisible, a stall is not.
  stamp_pair dup = make_stamp(src);
  out = drive_ok(timeline, dup);
  expect(out.pts == clamped + 2 * k_frame, "a duplicate stamp advances one frame");

  // A source that is merely behind by less than a frame is not a
  // discontinuity: it still lands one frame after last_out.
  std::puts("ok: backwards jump clamps");
}

// ---------------------------------------------------------------------------
// a forward jump larger than max_gap_ns is compressed to one frame
// ---------------------------------------------------------------------------
auto test_forward_jump_compresses() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  std::int64_t src = 0;
  std::int64_t last = 0;
  for (int idx = 0; idx < 10; ++idx) {
    last = drive_ok(timeline, make_stamp(src)).pts;
    src += k_frame;
  }

  // A resume 270 s later on the encoder's clock: a player would freeze for
  // 270 s if this were passed through.
  src += 270'000'000'000LL;
  stamp_pair out = drive_ok(timeline, make_stamp(src));
  expect(out.pts == last + k_frame, "a 270 s gap compresses to one frame");

  src += k_frame;
  out = drive_ok(timeline, make_stamp(src));
  expect(out.pts == last + 2 * k_frame, "the following buffer is one frame later");

  // A gap just under max_gap_ns is a normal step and passes through.
  src += k_max_gap - k_frame;
  const std::int64_t before = out.pts;
  out = drive_ok(timeline, make_stamp(src));
  expect(out.pts - before == k_max_gap - k_frame,
         "a gap below max_gap is passed through, not compressed");
  std::puts("ok: forward jump compresses");
}

// ---------------------------------------------------------------------------
// anchor(): the incoming source starts one frame after the last published one
// ---------------------------------------------------------------------------
auto test_anchor() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  std::int64_t src = 0;
  std::int64_t last = 0;
  for (int idx = 0; idx < 10; ++idx) {
    last = drive_ok(timeline, make_stamp(src)).pts;
    src += k_frame;
  }

  // A different source (the slate, then the new live feed) arrives with an
  // unrelated clock. Anchor before applying its first buffer.
  const std::int64_t new_src_first = 987'654'321'000LL;
  timeline.anchor(new_src_first);
  stamp_pair out = drive_ok(timeline, make_stamp(new_src_first));
  expect(out.pts == last + k_frame,
         "the docked source's first stamp lands one frame after last_out");

  // Its second buffer follows one frame later.
  out = drive_ok(timeline, make_stamp(new_src_first + k_frame));
  expect(out.pts == last + 2 * k_frame, "anchored stream continues at one frame");

  // Anchoring a fresh state adopts the source's own clock.
  timeline_state fresh(k_frame, k_max_gap);
  fresh.anchor(42 * k_frame);
  const stamp_pair first = drive_ok(fresh, make_stamp(42 * k_frame));
  expect(first.pts == 42 * k_frame, "a fresh anchor leaves the clock alone");
  std::puts("ok: anchor");
}

// ---------------------------------------------------------------------------
// one state shared by video and audio: both take the SAME offset
// ---------------------------------------------------------------------------
auto test_two_streams_share_offset() -> void
{
  timeline_state timeline(k_frame, k_max_gap);

  // Video drives the timeline (it is the stream the selector's decision is
  // made on); audio is rewritten with the same offset.
  std::int64_t video_src = 0;
  for (int idx = 0; idx < 5; ++idx) {
    drive_ok(timeline, make_stamp(video_src));
    video_src += k_frame;
  }

  // A switch: re-anchor on the new source's first video stamp.
  const std::int64_t new_video_first = 5'000'000'000LL;
  const std::int64_t new_audio_first = 5'000'400'000LL;  // not a video frame
  timeline.anchor(new_video_first);

  const stamp_pair video = drive_ok(timeline, make_stamp(new_video_first));
  const std::int64_t offset = timeline.offset_ns();

  // Audio's own buffer, shifted by the state's offset through the follower
  // entry point (its AAC frame is independent; it must not move last_out).
  std::int64_t audio_pts = new_audio_first;
  std::int64_t audio_dts = new_audio_first;  // audio: pts == dts
  expect(timeline.apply_follower(audio_pts, audio_dts), "audio accepted");
  expect(audio_pts - new_audio_first == video.pts - new_video_first,
         "video and audio took the same offset at the switch");
  expect(audio_dts == audio_pts,
         "audio DTS took the same offset (pts==dts preserved)");
  expect(video.pts - new_video_first == offset, "video offset is the state's offset");

  // A backwards clamp moves the shared offset for both streams alike: the next
  // audio buffer must move with the video, or lip-sync drifts at the switch.
  const std::int64_t clamped = drive_ok(
      timeline, make_stamp(new_video_first - 30 * k_frame)).pts;
  expect(clamped == video.pts + k_frame, "video clamped to last_out + one frame");
  const std::int64_t offset_after = timeline.offset_ns();
  expect(offset_after != offset, "the shared offset absorbed the clamp");

  std::int64_t next_audio_pts = new_audio_first + k_frame;
  std::int64_t next_audio_dts = next_audio_pts;
  expect(timeline.apply_follower(next_audio_pts, next_audio_dts),
         "audio accepted after the clamp");
  expect(next_audio_pts - (new_audio_first + k_frame) == offset_after,
         "audio follows the same offset after the clamp");
  expect(next_audio_pts - clamped >= 0,
         "audio stays on the published timeline after the clamp");
  std::puts("ok: two streams share one offset");
}

// ---------------------------------------------------------------------------
// the follower takes the driver's offset, clamps only against its OWN floor,
// and never touches the shared state
// ---------------------------------------------------------------------------
auto test_follower_never_moves_state() -> void
{
  timeline_state timeline(k_frame, k_max_gap);

  // Nothing published: a follower passes through untouched, like the driver.
  const stamp_pair first = follow_ok(timeline, make_stamp(100 * k_frame));
  expect(first.pts == 100 * k_frame, "a fresh follower passes through");
  expect(!timeline.published(),
         "a follower never publishes a buffer on the timeline");

  // Drive to a real offset, then anchor a new source.
  for (int idx = 0; idx < 20; ++idx) {
    drive_ok(timeline, make_stamp(idx * k_frame));
  }
  timeline.anchor(500'000'000LL);

  const std::int64_t last_before = timeline.last_out_ns();
  const std::int64_t offset_before = timeline.offset_ns();

  // (a) a follower buffer that steps forward on its own clock takes exactly the
  // current offset: no clamp, no compression, no state movement.
  std::int64_t ahead_pts = 101 * k_frame;
  std::int64_t ahead_dts = ahead_pts;
  expect(timeline.apply_follower(ahead_pts, ahead_dts), "follower ahead accepted");
  expect(ahead_pts == 101 * k_frame + offset_before,
         "the follower is shifted by exactly the current offset");
  expect(timeline.last_out_ns() == last_before,
         "a follower ahead of the driver does not move last_out");
  expect(timeline.offset_ns() == offset_before,
         "a follower ahead of the driver does not move the offset");
  const std::int64_t advanced_floor = ahead_pts;

  // (b) a follower buffer far BEHIND its own floor: bumped exactly one tick
  // past the FOLLOWER's floor -- never to the driver's last_out + frame, which
  // would silently lift a stream that is merely a little late.
  std::int64_t behind_pts = 0;  // on the new offset this is far behind
  std::int64_t behind_dts = 0;
  expect(timeline.apply_follower(behind_pts, behind_dts),
         "follower behind accepted");
  expect(behind_pts == advanced_floor + 1,
         "a backdated follower is clamped one tick past its own floor");
  expect(behind_pts != last_before + k_frame,
         "a backdated follower is not clamped to the driver's floor");
  expect(timeline.last_out_ns() == last_before,
         "a follower behind the driver does not move last_out");
  expect(timeline.offset_ns() == offset_before,
         "a follower behind the driver does not move the offset");

  // (c) the same source stamp twice: the duplicate advances one tick, not a
  // frame, and still never moves the shared state.
  std::int64_t dup_pts = 0;
  std::int64_t dup_dts = 0;
  expect(timeline.apply_follower(dup_pts, dup_dts), "duplicate accepted");
  expect(dup_pts == behind_pts + 1, "a duplicate advances exactly one tick");
  expect(timeline.last_out_ns() == last_before, "duplicate moved no last_out");
  expect(timeline.offset_ns() == offset_before, "duplicate moved no offset");

  // (d) a follower B-frame keeps its reorder distance.
  std::int64_t bframe_pts = dup_pts + 100 * k_frame;
  std::int64_t bframe_dts = bframe_pts - k_reorder;
  expect(timeline.apply_follower(bframe_pts, bframe_dts), "follower b-frame ok");
  expect(bframe_pts - bframe_dts == k_reorder,
         "a follower keeps its pts-dts delta");

  // And a malformed follower buffer is refused without moving anything.
  std::int64_t bad_pts = 1 * k_frame;
  std::int64_t bad_dts = 2 * k_frame;
  expect(!timeline.apply_follower(bad_pts, bad_dts),
         "a dts-after-pts follower is refused");
  expect(timeline.last_out_ns() == last_before, "refusal moved nothing");
  expect(timeline.offset_ns() == offset_before, "refusal moved no offset");
  expect(timeline.last_follower_ns() == bframe_pts,
         "refusal did not move the follower's own floor");
  std::puts("ok: follower never moves state");
}

// ---------------------------------------------------------------------------
// across a backwards driver clamp the follower moves forward with the offset
// and never steps back
// ---------------------------------------------------------------------------
auto test_follower_across_backwards_clamp() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  constexpr std::int64_t k_audio_frame = 21'333'333;  // 1024 @ 48 kHz

  std::int64_t video_src = 0;
  std::int64_t audio_src = 10 * k_audio_frame;
  for (int idx = 0; idx < 10; ++idx) {
    drive_ok(timeline, make_stamp(video_src));
    video_src += k_frame;
    follow_ok(timeline, {.pts = audio_src, .dts = audio_src});
    audio_src += k_audio_frame;
  }
  std::int64_t last_audio_out = audio_src - k_audio_frame;

  for (int jump = 0; jump < 3; ++jump) {
    // The source jumps back five frames; the driver clamps and lifts the
    // shared offset by the difference.
    video_src -= 5 * k_frame;
    const std::int64_t driver_before = timeline.last_out_ns();
    const stamp_pair video = drive_ok(timeline, make_stamp(video_src));
    expect(video.pts == driver_before + k_frame,
           "the driver clamps to last_out + one frame");
    video_src += k_frame;
    const std::int64_t offset = timeline.offset_ns();

    for (int idx = 0; idx < 4; ++idx) {
      const std::int64_t driver_floor = timeline.last_out_ns();
      std::int64_t pts = audio_src;
      std::int64_t dts = audio_src;
      expect(timeline.apply_follower(pts, dts), "audio accepted after the clamp");
      expect(pts == audio_src + offset,
             "each audio buffer takes exactly the driver's offset, unclamped");
      expect(pts > last_audio_out,
             "audio stays strictly monotonic across a backwards clamp");
      expect(timeline.offset_ns() == offset,
             "the follower never moves the shared offset");
      expect(timeline.last_out_ns() == driver_floor,
             "the follower never moves the driver's floor");
      last_audio_out = pts;
      audio_src += k_audio_frame;
    }
    // Let the video run a few frames before the next jump.
    for (int idx = 0; idx < 3; ++idx) {
      drive_ok(timeline, make_stamp(video_src));
      video_src += k_frame;
    }
  }
  std::puts("ok: follower across a backwards clamp");
}

// ---------------------------------------------------------------------------
// a forward-gap compression pulls the offset BACK; an audio buffer already
// stamped on the pre-gap clock must be bumped forward, not emitted backwards
// ---------------------------------------------------------------------------
auto test_follower_clamps_after_gap_compression() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  constexpr std::int64_t k_audio_frame = 21'333'333;

  // Settle both streams on one clock.
  std::int64_t video_src = 0;
  std::int64_t audio_src = 0;
  for (int idx = 0; idx < 20; ++idx) {
    drive_ok(timeline, make_stamp(video_src));
    video_src += k_frame;
    follow_ok(timeline, {.pts = audio_src, .dts = audio_src});
    audio_src += k_audio_frame;
  }
  const std::int64_t audio_floor = audio_src - k_audio_frame;
  expect(timeline.offset_ns() == 0, "a clean run leaves no offset");

  // The encoder vanishes for 270 s and resumes on its own clock: the driver
  // compresses the hole, which drags the shared offset 270 s backwards.
  video_src += 270'000'000'000LL;
  drive_ok(timeline, make_stamp(video_src));
  const std::int64_t offset = timeline.offset_ns();
  expect(offset <= -270'000'000'000LL,
         "the compression pulled the shared offset far backwards");
  const std::int64_t driver_floor = timeline.last_out_ns();

  // An audio AU already stamped on the old clock arrives after the switch. On
  // the new offset it would land 270 s before the last audio published: a
  // muxer would reject it. It must be bumped one tick past the audio floor.
  std::int64_t pts = audio_src;
  std::int64_t dts = audio_src;
  expect(timeline.apply_follower(pts, dts), "the in-flight audio AU is accepted");
  expect(pts == audio_floor + 1,
         "the in-flight audio AU is clamped one tick past its own floor");
  expect(pts == dts, "audio pts == dts is still honoured after the clamp");
  expect(timeline.offset_ns() == offset, "the clamp moved no shared offset");
  expect(timeline.last_out_ns() == driver_floor,
         "the clamp moved no driver floor");

  // A second pre-gap AU advances a tick, not a frame back.
  audio_src += k_audio_frame;
  std::int64_t pts2 = audio_src;
  std::int64_t dts2 = audio_src;
  expect(timeline.apply_follower(pts2, dts2), "the second AU is accepted");
  expect(pts2 == pts + 1, "each backdated AU advances exactly one tick");

  // Once the audio source clock has also jumped the gap, its buffers line up
  // again naturally: the clamp cost the in-flight frames only.
  const std::int64_t caught_up = audio_src + 270'000'000'000LL;
  std::int64_t pts3 = caught_up;
  std::int64_t dts3 = caught_up;
  expect(timeline.apply_follower(pts3, dts3), "the post-gap AU is accepted");
  expect(pts3 == caught_up + offset,
         "a post-gap AU takes the offset with no clamp");
  expect(pts3 > pts2, "the follower continues forward after the switch");
  expect(timeline.offset_ns() == offset, "the run moved no shared offset");
  std::puts("ok: follower clamps after a gap compression");
}

// ---------------------------------------------------------------------------
// 100 pseudo-random driver steps with audio in lock-step: the follower output
// is strictly increasing and always equals the driver's offset or its floor+1
// ---------------------------------------------------------------------------
auto test_follower_monotonic_under_random_steps() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  constexpr std::int64_t k_audio_frame = 21'333'333;

  std::uint32_t rng = 0x2545F491U;
  auto next_rand = [&rng]() -> std::uint32_t
  {
    rng ^= rng << 13U;
    rng ^= rng >> 17U;
    rng ^= rng << 5U;
    return rng;
  };

  std::int64_t video_src = 50 * k_frame;
  std::int64_t audio_src = 50 * k_frame;
  std::int64_t prev_audio_out = 0;
  bool have_audio = false;
  int bumps = 0;
  int natural = 0;

  for (int idx = 0; idx < 100; ++idx) {
    const std::uint32_t pick = next_rand() % 100U;
    std::int64_t step = k_frame;
    if (pick < 30U) {
      step = -static_cast<std::int64_t>(1U + next_rand() % 200U) * k_frame;
    } else if (pick < 55U) {
      step = static_cast<std::int64_t>(1U + next_rand() % 600U)
             * 1'000'000'000LL;
    } else if (pick < 65U) {
      step = 0;
    }
    video_src += step;
    drive_ok(timeline, make_stamp(video_src));

    const std::int64_t offset = timeline.offset_ns();
    const std::int64_t driver_floor = timeline.last_out_ns();

    for (int tick = 0; tick < 2; ++tick) {
      std::int64_t pts = audio_src;
      std::int64_t dts = audio_src;
      expect(timeline.apply_follower(pts, dts), "random follower accepted");
      const std::int64_t wanted = audio_src + offset;
      if (have_audio && wanted <= prev_audio_out) {
        expect(pts == prev_audio_out + 1,
               "a backdated audio buffer is bumped exactly one tick");
        ++bumps;
      } else {
        expect(pts == wanted, "an in-order audio buffer takes the offset");
        ++natural;
      }
      if (have_audio) {
        expect(pts > prev_audio_out,
               "the follower is strictly monotonic under random churn");
      }
      expect(pts >= audio_src + offset, "the follower never moves backwards");
      expect(timeline.offset_ns() == offset,
             "the follower never moves the shared offset");
      expect(timeline.last_out_ns() == driver_floor,
             "the follower never moves the driver's floor");
      prev_audio_out = pts;
      have_audio = true;
      audio_src += k_audio_frame;
    }
  }
  expect(bumps > 0 && natural > 0,
         "the random run hit both the clamp and the pass-through path");
  std::printf("ok: follower monotonic under random steps (%d bumps, %d natural)\n",
              bumps, natural);
}

// ---------------------------------------------------------------------------
// audio stays monotonic while the video timeline clamps three backwards jumps
// ---------------------------------------------------------------------------
auto test_follower_monotonic_under_clamps() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  constexpr std::int64_t k_audio_frame = 21'333'333;  // 1024 samples @ 48 kHz

  std::int64_t video_src = 0;
  std::int64_t audio_src = 0;
  std::int64_t last_audio_out = 0;

  // A normal run first, so both streams have a published position.
  for (int idx = 0; idx < 10; ++idx) {
    drive_ok(timeline, make_stamp(video_src));
    video_src += k_frame;
    std::int64_t pts = audio_src;
    std::int64_t dts = audio_src;
    follow_ok(timeline, {.pts = pts, .dts = dts});
    audio_src += k_audio_frame;
  }
  {
    std::int64_t pts = audio_src;
    std::int64_t dts = audio_src;
    follow_ok(timeline, {.pts = pts, .dts = dts});
    last_audio_out = pts;
    audio_src += k_audio_frame;
  }

  // Three backwards jumps of five frames each on the video side: each clamps
  // and shifts the shared offset. Audio just keeps walking forward on its own
  // clock, and must stay strictly monotonic through every shift.
  for (int jump = 0; jump < 3; ++jump) {
    video_src -= 5 * k_frame;
    for (int idx = 0; idx < 3; ++idx) {
      drive_ok(timeline, make_stamp(video_src));
      video_src += k_frame;
    }
    const std::int64_t offset = timeline.offset_ns();
    for (int idx = 0; idx < 4; ++idx) {
      const stamp_pair out =
          follow_ok(timeline, {.pts = audio_src, .dts = audio_src});
      expect(out.pts - audio_src == offset,
             "each audio buffer takes the driver's current offset");
      expect(out.pts > last_audio_out,
             "audio stays strictly monotonic across a video clamp");
      last_audio_out = out.pts;
      audio_src += k_audio_frame;
    }
  }
  std::puts("ok: follower monotonic under clamps");
}

// ---------------------------------------------------------------------------
// a malformed buffer is refused, and a refused buffer does not move the state
// ---------------------------------------------------------------------------
auto test_malformed_refused() -> void
{
  timeline_state timeline(k_frame, k_max_gap);
  const stamp_pair good = drive_ok(timeline, make_stamp(0));
  const std::int64_t offset_before = timeline.offset_ns();

  std::int64_t pts = 10 * k_frame;
  std::int64_t dts = 20 * k_frame;  // dts after pts: not a legal buffer
  expect(!timeline.apply_driver(pts, dts), "a dts-after-pts buffer is refused");
  expect(timeline.offset_ns() == offset_before, "a refused buffer changes no offset");
  expect(timeline.last_out_ns() == good.pts, "a refused buffer does not move last_out");

  // The timeline still works afterwards.
  const stamp_pair next = drive_ok(timeline, make_stamp(k_frame));
  expect(next.pts == good.pts + k_frame, "the timeline continues after a refusal");
  std::puts("ok: malformed refused");
}

// ---------------------------------------------------------------------------
// 1000 pseudo-random source steps: the output is strictly monotonic, and every
// step is either the source step or exactly one frame
// ---------------------------------------------------------------------------
auto test_monotonic_under_churn() -> void
{
  timeline_state timeline(k_frame, k_max_gap);

  // xorshift32: deterministic, no <random> dependency.
  std::uint32_t rng = 0x9E3779B9U;
  auto next_rand = [&rng]() -> std::uint32_t
  {
    rng ^= rng << 13U;
    rng ^= rng >> 17U;
    rng ^= rng << 5U;
    return rng;
  };

  std::int64_t src = 12 * k_frame;
  std::int64_t last_out = 0;
  std::uint32_t last_pts = 0;
  (void)last_pts;
  std::int64_t prev_out = 0;
  int backwards = 0;
  int huge = 0;
  int normal = 0;

  for (int idx = 0; idx < 1000; ++idx) {
    const std::uint32_t pick = next_rand() % 100U;
    std::int64_t step = k_frame;
    if (pick < 25U) {
      // backwards jump of 1..120 frames
      step = -static_cast<std::int64_t>(1U + next_rand() % 120U) * k_frame;
      ++backwards;
    } else if (pick < 40U) {
      // huge forward jump: 1..600 s
      step = static_cast<std::int64_t>(1U + next_rand() % 600U)
             * 1'000'000'000LL;
      ++huge;
    } else {
      // normal step, occasionally a duplicate or a small jitter
      const std::uint32_t jitter = next_rand() % 10U;
      if (jitter == 0U) {
        step = 0;  // duplicate stamp
      } else if (jitter == 1U) {
        step = -k_frame / 2;  // slightly behind
      }
      ++normal;
    }
    src += step;

    stamp_pair out = make_stamp(src);
    expect(timeline.apply_driver(out.pts, out.dts), "churn buffer accepted");
    expect(out.pts - out.dts == k_reorder, "delta preserved throughout churn");
    if (idx != 0) {
      expect(out.pts > prev_out, "output is strictly monotonic");
      const std::int64_t delta = out.pts - prev_out;
      const bool clamped = delta == k_frame;
      const bool passed = delta == step && step > 0 && step <= k_max_gap;
      expect(clamped || passed,
             "every step is either the source step or exactly one frame");
      expect(delta <= k_max_gap, "no output step exceeds max_gap");
    }
    prev_out = out.pts;
    last_out = out.pts;
  }
  expect(backwards > 0 && huge > 0 && normal > 0, "the churn hit every path");
  expect(timeline.last_out_ns() == last_out, "last_out is the final output stamp");
  std::printf("ok: monotonic under churn (%d backwards, %d huge, %d normal)\n",
              backwards, huge, normal);
}
}  // namespace

auto main() -> int
{
  test_first_buffer_passes_through();
  test_normal_step_passes_through();
  test_bframe_delta_preserved();
  test_backwards_jump_clamps();
  test_forward_jump_compresses();
  test_anchor();
  test_two_streams_share_offset();
  test_follower_never_moves_state();
  test_follower_across_backwards_clamp();
  test_follower_clamps_after_gap_compression();
  test_follower_monotonic_under_random_steps();
  test_follower_monotonic_under_clamps();
  test_malformed_refused();
  test_monotonic_under_churn();
  std::puts("timeline_test: ALL OK");
  return 0;
}