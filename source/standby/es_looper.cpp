// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// es_looper — the pure AU splitter, plus the replay thread that pushes a
// rendered slate into an appsrc forever (plan B2).
//
// Two rules drive the whole file:
//   1. Splice only at an access-unit boundary. A mid-AU splice is visible
//      corruption; a boundary splice is invisible because the next AU is a
//      complete picture with a PTS of its own.
//   2. Never write a timestamp that goes backwards. The timeline is the
//      session frame clock counted across passes, so the loop point is
//      indistinguishable from any other frame to the re-stamper downstream.

#include "standby/es_looper.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

#ifdef OBR_HAVE_GSTREAMER
#include <gst/app/gstappsrc.h>
#endif

namespace standby
{
namespace
{
constexpr std::size_t k_npos = static_cast<std::size_t>(-1);

// Length of the Annex-B start code at `pos` (3 or 4), or 0 when there is none.
// The length matters: the NAL header is the byte *after* the whole code, so a
// 4-byte code read as 3 leaves the reader looking at its own 0x01.
auto start_code_len(const std::uint8_t* data, std::size_t len, std::size_t pos)
    -> std::size_t
{
  if (pos + 2 >= len || data[pos] != 0 || data[pos + 1] != 0) {
    return 0;
  }
  if (data[pos + 2] == 1) {
    return 3;
  }
  if (pos + 3 < len && data[pos + 2] == 0 && data[pos + 3] == 1) {
    return 4;
  }
  return 0;
}

struct obu_view
{
  std::uint8_t type = 0;
  std::size_t payload = 0;  // offset of the first payload byte
  std::size_t end = 0;      // offset just past the OBU
};

// Parse one OBU at `pos`. False when the framing does not parse (a truncated
// tail, or a stream that is not OBU-framed at all).
auto parse_obu(const std::uint8_t* data, std::size_t len, std::size_t pos,
               obu_view& out) -> bool
{
  if (pos >= len) {
    return false;
  }
  const std::uint8_t header = data[pos];
  out.type = static_cast<std::uint8_t>((header >> 3) & 0x0f);
  const bool extension = (header & 0x04) != 0;
  const bool has_size = (header & 0x02) != 0;
  std::size_t cursor = pos + 1 + (extension ? 1u : 0u);
  if (cursor > len) {
    return false;
  }
  out.payload = cursor;
  if (!has_size) {
    // No size field: an OBU the extent of which is implied (a temporal
    // delimiter, in practice, which carries no payload).
    out.end = cursor;
    return true;
  }
  std::uint64_t size = 0;
  int shift = 0;
  bool complete = false;
  while (cursor < len && shift < 64) {
    const std::uint8_t byte = data[cursor++];
    size |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) {
      complete = true;
      break;
    }
  }
  if (!complete) {
    return false;
  }
  out.payload = cursor;
  if (size > len - cursor) {
    return false;
  }
  out.end = cursor + static_cast<std::size_t>(size);
  return true;
}
}  // namespace

auto split_access_units(const std::uint8_t* data, std::size_t len)
    -> std::vector<au_span>
{
  struct nal
  {
    std::size_t start;    // offset of the start code
    std::uint8_t header;  // the NAL header byte that follows it
  };
  std::vector<nal> nals;
  if (data == nullptr || len < 3) {
    return {};
  }
  for (std::size_t pos = 0; pos + 2 < len; ++pos) {
    const std::size_t code = start_code_len(data, len, pos);
    if (code == 0) {
      continue;
    }
    const std::size_t header_at = pos + code;
    if (header_at >= len) {
      break;
    }
    nals.push_back(nal {pos, data[header_at]});
    pos += code - 1;  // the loop increment lands on the NAL header
  }
  if (nals.empty()) {
    return {};
  }

  // h264 AUD is type 9, h265 AUD is type 35. The two byte values cannot
  // collide, so one test covers both without a codec argument.
  const auto is_aud = [](const nal& n) {
    return (n.header & 0x1f) == 9 || ((n.header >> 1) & 0x3f) == 35;
  };
  const bool has_aud = std::any_of(nals.begin(), nals.end(), is_aud);

  std::vector<au_span> aus;
  if (!has_aud) {
    // One NAL per unit: still a legal push for h264/h265, and it can never cut
    // a NAL in half.
    for (std::size_t i = 0; i < nals.size(); ++i) {
      const std::size_t end = i + 1 < nals.size() ? nals[i + 1].start : len;
      aus.emplace_back(nals[i].start, end - nals[i].start);
    }
    return aus;
  }

  // An access unit is the group of NALs between access-unit delimiters, which
  // is the grouping the encoder itself declared.
  for (std::size_t i = 0; i < nals.size(); ++i) {
    if (!is_aud(nals[i])) {
      continue;
    }
    std::size_t end = len;
    for (std::size_t j = i + 1; j < nals.size(); ++j) {
      if (is_aud(nals[j])) {
        end = nals[j].start;
        break;
      }
    }
    aus.emplace_back(nals[i].start, end - nals[i].start);
  }
  return aus;
}

auto split_obu_access_units(const std::uint8_t* data, std::size_t len)
    -> std::vector<au_span>
{
  std::vector<au_span> aus;
  if (data == nullptr || len == 0) {
    return aus;
  }

  std::size_t au_start = k_npos;
  std::size_t pos = 0;
  while (pos < len) {
    obu_view obu;
    if (!parse_obu(data, len, pos, obu)) {
      break;  // truncated tail stays with the current AU
    }
    if (obu.type == 2) {  // OBU_TEMPORAL_DELIMITER opens an AU
      if (au_start != k_npos && pos > au_start) {
        aus.emplace_back(au_start, pos - au_start);
      }
      au_start = pos;
    }
    pos = obu.end > pos ? obu.end : pos + 1;
  }
  if (au_start != k_npos) {
    aus.emplace_back(au_start, len - au_start);
  }
  return aus;
}

auto au_is_keyframe(es_codec cod, const std::uint8_t* au, std::size_t len)
    -> bool
{
  if (au == nullptr || len < 3) {
    return false;
  }
  // Every NAL in the unit is examined, not just the first: an encoder that
  // emits an access-unit delimiter (x264enc and the VA encoders both do) puts
  // the delimiter first, so "the first NAL is not an IDR" says nothing about
  // the picture. A unit is a decoder entry point when it carries a parameter
  // set or an IDR/IRAP slice anywhere inside it.
  for (std::size_t pos = 0; pos + 2 < len; ++pos) {
    const std::size_t code = start_code_len(au, len, pos);
    if (code == 0) {
      continue;
    }
    const std::size_t header_at = pos + code;
    pos += code - 1;
    if (header_at >= len) {
      break;
    }
    const std::uint8_t header = au[header_at];
    if (cod == es_codec::h264) {
      // NAL unit type in the low 5 bits: 5 = IDR slice, 7 = SPS. 9 (AUD) is
      // excluded on purpose: it prefixes units regardless of their type.
      const std::uint8_t type = header & 0x1f;
      if (type == 5 || type == 7) {
        return true;
      }
    } else if (cod == es_codec::h265) {
      // 6-bit type in bits 1..6; 16..21 are the IRAP pictures (BLA/IDR/CRA) and
      // 32..34 are VPS/SPS/PPS. 35 (AUD) is excluded for the same reason.
      const std::uint8_t type = (header >> 1) & 0x3f;
      if ((type >= 16 && type <= 21) || (type >= 32 && type <= 34)) {
        return true;
      }
    } else {
      return false;  // av1 is OBU-framed; see obu_au_is_keyframe
    }
  }
  return false;
}

auto obu_au_is_keyframe(const std::uint8_t* au, std::size_t len) -> bool
{
  if (au == nullptr || len == 0) {
    return false;
  }
  std::size_t pos = 0;
  while (pos < len) {
    obu_view obu;
    if (!parse_obu(au, len, pos, obu)) {
      return false;
    }
    if (obu.type == 3 || obu.type == 6) {  // OBU_FRAME_HEADER / OBU_FRAME
      // The payload opens with show_existing_frame(1) then frame_type(2);
      // KEY_FRAME is 0. A show-existing frame presents an earlier one and is
      // not a decoder entry point.
      if (obu.payload >= len) {
        return false;
      }
      const std::uint8_t byte = au[obu.payload];
      if ((byte & 0x80) != 0) {
        return false;
      }
      return ((byte >> 5) & 0x03) == 0;
    }
    pos = obu.end > pos ? obu.end : pos + 1;
  }
  return false;
}

#ifdef OBR_HAVE_GSTREAMER

namespace
{
// The slate is two seconds of a still; reading it whole is simpler than
// seeking and lets the AU split be done once per session rather than per pass.
auto read_whole_file(const std::string& path) -> std::vector<std::uint8_t>
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::vector<std::uint8_t> {std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
}

// The codec is read from the file extension, which the renderer owns. It has
// to be known, not guessed: an h264 non-IDR slice and an h265 VPS share a NAL
// header byte, so no single reading of the header works for both.
auto codec_from_path(const std::string& path, es_codec& out) -> bool
{
  const auto ends_with = [&path](std::string_view suffix) {
    return path.size() >= suffix.size()
        && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  if (ends_with(".av1")) {
    out = es_codec::av1;
    return true;
  }
  if (ends_with(".h265") || ends_with(".hevc")) {
    out = es_codec::h265;
    return true;
  }
  if (ends_with(".h264")) {
    out = es_codec::h264;
    return true;
  }
  return false;
}
}  // namespace

es_looper::es_looper(GstElement* appsrc, std::string path,
                     std::int64_t frame_duration_ns, log_fn log)
    : m_appsrc(appsrc != nullptr ? GST_ELEMENT(gst_object_ref(appsrc)) : nullptr),
      m_path(std::move(path)),
      // 33 ms: 30 fps. A wrong pace shows as a slate that runs slightly fast or
      // slow, which is invisible; no pace at all is a flood.
      m_frame_duration_ns(frame_duration_ns > 0 ? frame_duration_ns : 33000000),
      m_log(std::move(log))
{
}

es_looper::~es_looper()
{
  stop();
}

auto es_looper::start() -> bool
{
  if (m_running.exchange(true)) {
    return true;  // already looping
  }
  if (m_appsrc == nullptr) {
    note("standby: es_looper has no appsrc; not looping");
    m_running.store(false);
    return false;
  }

  // Load and split here, not on the thread: the caller needs to know now
  // whether the slate is usable, and it is two seconds of ES either way.
  m_data = read_whole_file(m_path);
  if (m_data.empty()) {
    note(std::format("standby: slate ES '{}' is unreadable or empty; "
                     "no slate will be pushed", m_path));
    m_running.store(false);
    return false;
  }
  if (!codec_from_path(m_path, m_codec)) {
    // Unknown suffix: assume Annex-B h264, the overwhelmingly common case, and
    // say so once rather than refusing to loop a slate that renders fine.
    m_codec = es_codec::h264;
    note(std::format("standby: slate ES '{}' has no known codec suffix; "
                     "reading it as Annex-B h264", m_path));
  }
  m_aus = m_codec == es_codec::av1
      ? split_obu_access_units(m_data.data(), m_data.size())
      : split_access_units(m_data.data(), m_data.size());
  if (m_aus.empty()) {
    note(std::format("standby: slate ES '{}' has no access units", m_path));
    m_data.clear();
    m_running.store(false);
    return false;
  }

  // The switch presents this branch at whatever access unit the loop has
  // reached, so every one of them has to be independently decodable. Count the
  // ones that are not, and say so once: a slate rendered by this build is
  // all-intra, so a non-zero count means the file was produced elsewhere (or by
  // an older cache) and a standby press can land on a picture with no
  // references.
  std::size_t non_keyframes = 0;
  for (const au_span& span : m_aus) {
    const std::uint8_t* au = m_data.data() + span.first;
    const bool keyframe = m_codec == es_codec::av1
        ? obu_au_is_keyframe(au, span.second)
        : au_is_keyframe(m_codec, au, span.second);
    non_keyframes += keyframe ? 0 : 1;
  }
  if (non_keyframes != 0) {
    note(std::format(
        "standby: slate ES '{}' is not all-intra ({} of {} access units are "
        "not keyframes); a standby press can start the slate mid-GOP",
        m_path, non_keyframes, m_aus.size()));
  }

  m_thread = std::thread([this] { run(); });
  return true;
}

auto es_looper::note(const std::string& message) const -> void
{
  if (m_log) {
    m_log(message);
  }
}

auto es_looper::stop() -> void
{
  // Idempotent: the first caller flips the flag and wakes the pacing wait, every
  // caller joins whatever thread exists.
  const bool was_running = m_running.exchange(false);
  if (was_running) {
    m_cv.notify_all();
  }
  if (m_thread.joinable()) {
    m_thread.join();
  }
  if (m_appsrc != nullptr) {
    gst_object_unref(m_appsrc);
    m_appsrc = nullptr;
  }
}

auto es_looper::pushed_access_units() const -> std::uint64_t
{
  return m_pushed.load(std::memory_order_relaxed);
}

auto es_looper::loops_completed() const -> std::uint64_t
{
  return m_loops.load(std::memory_order_relaxed);
}

auto es_looper::run() -> void
{
  if (m_data.empty() || m_aus.empty()) {
    return;
  }

  const auto frame_duration = static_cast<GstClockTime>(m_frame_duration_ns);
  const auto epoch = std::chrono::steady_clock::now();
  GstClockTime pts = 0;
  std::uint64_t loops = 0;

  while (m_running.load(std::memory_order_relaxed)) {
    for (const auto& [offset, length] : m_aus) {
      if (!m_running.load(std::memory_order_relaxed)) {
        return;
      }

      // Pace to the session frame clock instead of pushing as fast as the loop
      // can copy: appsrc is block=false (the fan-out convention), so an
      // unpaced push either floods the pipeline or, worse, is dropped
      // downstream and starves the muxer. wait_until so stop() is immediate.
      const auto deadline =
          epoch + std::chrono::nanoseconds {static_cast<std::int64_t>(pts)};
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_cv.wait_until(lock, deadline, [this] {
              return !m_running.load(std::memory_order_relaxed);
            }))
        {
          return;
        }
      }

      GstBuffer* buffer = gst_buffer_new_allocate(nullptr, length, nullptr);
      if (buffer == nullptr) {
        note("standby: es_looper could not allocate a buffer; looping stopped");
        return;
      }
      gst_buffer_fill(buffer, 0, m_data.data() + offset, length);
      GST_BUFFER_PTS(buffer) = pts;
      // DTS is left unset: the slate is re-stamped downstream from PTS alone,
      // and inventing a DTS here would be a second authority on presentation
      // order for a stream that has no reordering.
      GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_DURATION(buffer) = frame_duration;
      const bool keyframe = m_codec == es_codec::av1
          ? obu_au_is_keyframe(m_data.data() + offset, length)
          : au_is_keyframe(m_codec, m_data.data() + offset, length);
      if (!keyframe) {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
      }

      const auto flow = gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), buffer);
      if (flow == GST_FLOW_FLUSHING) {
        return;  // the pipeline went to NULL under us: not an error
      }
      if (flow != GST_FLOW_OK) {
        note(std::format("standby: es_looper push failed ({})",
                         gst_flow_get_name(flow)));
        return;
      }

      pts += frame_duration;
      m_pushed.fetch_add(1, std::memory_order_relaxed);
    }

    // EOF: rewind in place. The next pass restarts at the first AU, which is
    // the IDR/KEY_FRAME the renderer encoded first, so nothing downstream ever
    // sees a delta frame without its reference.
    ++loops;
    m_loops.store(loops, std::memory_order_relaxed);
    if (loops == 1) {
      note(std::format("standby: es_looper looping '{}' ({} access units, "
                       "{} ns per frame)", m_path, m_aus.size(),
                       m_frame_duration_ns));
    }
  }
}

#endif  // OBR_HAVE_GSTREAMER

}  // namespace standby
