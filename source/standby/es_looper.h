// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_ES_LOOPER_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_ES_LOOPER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef OBR_HAVE_GSTREAMER
#include <gst/gst.h>
#endif

namespace standby
{

// An access unit's location in the file: [offset, offset + length). Two
// functions, one per elementary-stream framing the renderer can produce. Both
// are pure and unit-tested without a media stack: an AU boundary is the one
// thing the loop must never get wrong, because a splice inside an AU is
// visible corruption on air while a splice at a boundary is invisible.
using au_span = std::pair<std::size_t, std::size_t>;

// Annex-B (h264, h265). An access unit is the group of NALs between access-unit
// delimiters: when the stream carries AUDs -- x264enc and the VA encoders both
// emit them, so every file the renderer produces does -- the units are the
// groups between them. A stream with no AUDs falls back to one NAL per unit,
// which is still a legal push and, more importantly, can never cut a NAL in
// half. Bytes before the first start code are skipped: they are not a unit.
auto split_access_units(const std::uint8_t* data, std::size_t len)
    -> std::vector<au_span>;

// AV1 "obu-stream" (what av1parse emits and what mpegtsmux/tsdemux carry):
// OBUs are concatenated header+leb128-size+payload with no start codes, and a
// temporal-delimiter OBU (type 2) opens every access unit. Splitting walks the
// OBU framing rather than searching for a byte pattern, so a payload that
// happens to contain a delimiter-looking sequence cannot split an AU.
auto split_obu_access_units(const std::uint8_t* data, std::size_t len)
    -> std::vector<au_span>;

// The elementary-stream codec of a slate file. Deliberately a separate small
// enum rather than reuse of slate_codec: the looper needs it only to read a NAL
// header, and the two readings of that header overlap (an h264 non-IDR slice
// byte 0x41 is an h265 VPS type 32), so the codec can never be guessed.
enum class es_codec : std::uint8_t
{
  h264,
  h265,
  av1
};

// True when an Annex-B AU of `cod` carries a parameter set (SPS/VPS/PPS) or an
// IDR/IRAP slice -- i.e. a decoder can start here, which is what makes the loop
// point invisible. Every NAL in the unit is examined, because the first one is
// usually an access-unit delimiter rather than the picture. h264 and h265 are
// read separately: their NAL header layouts share the same byte.
auto au_is_keyframe(es_codec cod, const std::uint8_t* au, std::size_t len)
    -> bool;

// True when an OBU-framed AU carries a KEY_FRAME (not a show-existing-frame).
auto obu_au_is_keyframe(const std::uint8_t* au, std::size_t len) -> bool;

#ifdef OBR_HAVE_GSTREAMER

// es_looper — replays a rendered ES file into an appsrc forever.
//
// It is the runtime half of the slate: the renderer (B1) produces two seconds
// of ES once per session, and this pushes that file round and round with a
// timeline that never rewinds. That matters more than it looks: the downstream
// re-stamper and ring only ever see a monotonically increasing PTS, so the
// loop point is indistinguishable from any other frame to every destination.
//
// On its own thread, paced to real time (the appsrc is block=false, per the
// fan-out convention, so an unpaced loop would either flood the pipeline or
// starve it). Each unit is pushed with PTS = n * frame_duration and no DTS, and
// flagged DELTA_UNIT unless it carries an IDR/IRAP -- so whatever switches to
// this branch can land on the loop point and find a keyframe there.
// stop() is idempotent, joins promptly, and a flushing appsrc ends the loop
// without a retry storm. The codec is taken from the file's extension.
class es_looper
{
public:
  using log_fn = std::function<void(const std::string&)>;

  // `appsrc` must be an appsrc; the looper takes its own reference, so the
  // caller may drop the pipeline from under it and stop()/the destructor will
  // release that reference. `frame_duration_ns` is one frame of the session's
  // frame clock (0 falls back to 33 ms: a slate at the wrong pace is better
  // than a division by zero).
  es_looper(GstElement* appsrc, std::string path,
            std::int64_t frame_duration_ns, log_fn log = {});
  ~es_looper();

  es_looper(const es_looper&) = delete;
  auto operator=(const es_looper&) -> es_looper& = delete;
  es_looper(es_looper&&) = delete;
  auto operator=(es_looper&&) -> es_looper& = delete;

  // False when there is nothing to loop (no appsrc, file unreadable, no AUs);
  // the reason has been logged. The caller treats that as a degraded slate.
  auto start() -> bool;
  auto stop() -> void;

  auto pushed_access_units() const -> std::uint64_t;
  auto loops_completed() const -> std::uint64_t;

private:
  auto run() -> void;
  auto note(const std::string& message) const -> void;

  GstElement* m_appsrc = nullptr;
  std::string m_path;
  std::int64_t m_frame_duration_ns = 0;
  log_fn m_log;

  // Loaded by start() so a broken file is reported to the caller instead of
  // disappearing on a thread: the session marks itself degraded, it does not
  // discover the problem as silence.
  std::vector<std::uint8_t> m_data;
  std::vector<au_span> m_aus;
  es_codec m_codec = es_codec::h264;

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::atomic<bool> m_running {false};
  std::atomic<std::uint64_t> m_pushed {0};
  std::atomic<std::uint64_t> m_loops {0};
};

#endif  // OBR_HAVE_GSTREAMER

}  // namespace standby

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_ES_LOOPER_H
