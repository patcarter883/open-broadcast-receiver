// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "standby/stream_switch.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#ifdef OBR_HAVE_GSTREAMER
#include <gst/gst.h>
#endif

namespace standby
{
namespace
{
// A NAL start code is 00 00 01; a 4-byte code is the same with a leading zero
// that the payload trim below absorbs.
auto find_start_code(const std::uint8_t* data, std::size_t len,
                     std::size_t from) -> std::size_t
{
  for (std::size_t i = from; i + 3 <= len; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      return i;
    }
  }
  return len;
}

// Walk the NALs of an Annex-B buffer, stopping early when fn returns true.
template <typename Fn>
auto scan_annexb(const std::uint8_t* data, std::size_t len, Fn&& fn) -> bool
{
  std::size_t pos = 0;
  while (true) {
    const std::size_t sc = find_start_code(data, len, pos);
    if (sc >= len) {
      return false;
    }
    const std::size_t payload = sc + 3;
    const std::size_t next = find_start_code(data, len, payload);
    std::size_t end = next;
    // Trailing zeros are the leading zero of the following 4-byte start code,
    // not part of this NAL.
    while (end > payload && data[end - 1] == 0) {
      --end;
    }
    if (end > payload && fn(data[payload], data + payload, end - payload)) {
      return true;
    }
    pos = payload;
  }
}

// The same walk for avc/avc3: 4-byte big-endian lengths.
template <typename Fn>
auto scan_length_prefixed(const std::uint8_t* data, std::size_t len, Fn&& fn)
    -> bool
{
  std::size_t pos = 0;
  while (pos + 4 <= len) {
    const std::uint32_t nal_len = (static_cast<std::uint32_t>(data[pos]) << 24U)
                                  | (static_cast<std::uint32_t>(data[pos + 1])
                                     << 16U)
                                  | (static_cast<std::uint32_t>(data[pos + 2])
                                     << 8U)
                                  | static_cast<std::uint32_t>(data[pos + 3]);
    pos += 4;
    if (nal_len == 0 || nal_len > len - pos) {
      return false;
    }
    if (fn(data[pos], data + pos, static_cast<std::size_t>(nal_len))) {
      return true;
    }
    pos += nal_len;
  }
  return false;
}

auto read_leb128(const std::uint8_t* data, std::size_t len, std::size_t& pos,
                 std::size_t& out) -> bool
{
  std::size_t value = 0;
  for (int idx = 0; idx < 8; ++idx) {
    if (pos >= len) {
      return false;
    }
    const std::uint8_t byte = data[pos];
    ++pos;
    value |= static_cast<std::size_t>(byte & 0x7FU) << (idx * 7);
    if ((byte & 0x80U) == 0) {
      out = value;
      return true;
    }
  }
  return false;
}

// AV1 OBU framing: a keyframe is a keyframe-aligned OBU stream (a sequence
// header is present) or a first frame OBU whose uncompressed header says
// frame_type == KEY_FRAME and not show_existing_frame.
auto obu_starts_keyframe(const std::uint8_t* data, std::size_t len) -> bool
{
  std::size_t pos = 0;
  while (pos < len) {
    const std::uint8_t header = data[pos];
    ++pos;
    if ((header & 0x80U) != 0) {  // forbidden bit: not a legal OBU stream
      return false;
    }
    const int type = (header >> 3) & 0x0F;
    const bool extension = (header & 0x04U) != 0;
    const bool has_size = (header & 0x02U) != 0;
    if (extension) {
      if (pos >= len) {
        return false;
      }
      ++pos;
    }
    std::size_t size = 0;
    if (has_size) {
      if (!read_leb128(data, len, pos, size)) {
        return false;
      }
    } else {
      size = len - pos;  // last OBU: it runs to the end of the buffer
    }
    if (size > len - pos) {
      size = len - pos;  // tolerate a truncated tail; the header is still usable
    }
    if (type == 1) {  // OBU_SEQUENCE_HEADER: emitted ahead of every keyframe
      return true;
    }
    if (type == 3 || type == 6) {  // OBU_FRAME_HEADER / OBU_FRAME
      if (size == 0) {
        return false;
      }
      const std::uint8_t first = data[pos];
      if ((first & 0x80U) != 0) {  // show_existing_frame: not a new keyframe
        return false;
      }
      return ((first >> 5) & 0x03) == 0;  // frame_type: 0 = KEY_FRAME
    }
    pos += size;
  }
  return false;
}

#ifdef OBR_HAVE_GSTREAMER
auto default_log(const std::string& message) -> void
{
  std::fputs(message.c_str(), stderr);
  if (message.empty() || message.back() != '\n') {
    std::fputc('\n', stderr);
  }
}

// Find a pad by name, including request pads (gst_element_get_static_pad warns
// on those). Returns a new reference or nullptr.
auto find_pad(GstElement* element, const char* name) -> GstPad*
{
  GstPad* found = nullptr;
  GstIterator* iter = gst_element_iterate_pads(element);
  GValue value = G_VALUE_INIT;
  while (gst_iterator_next(iter, &value) == GST_ITERATOR_OK) {
    auto* pad = GST_PAD(g_value_get_object(&value));
    if (pad != nullptr && g_strcmp0(GST_PAD_NAME(pad), name) == 0) {
      found = GST_PAD(gst_object_ref(pad));
      g_value_reset(&value);
      break;
    }
    g_value_reset(&value);
  }
  g_value_unset(&value);
  gst_iterator_free(iter);
  return found;
}

// The branch's caps: what the pad has negotiated, else what it is configured
// to negotiate (attach can happen before PLAYING).
auto branch_caps(GstPad* pad, const std::string& configured) -> GstCaps*
{
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps != nullptr) {
    return caps;
  }
  if (!configured.empty()) {
    return gst_caps_from_string(configured.c_str());
  }
  return nullptr;
}

auto caps_codec(GstCaps* caps, es_frame_codec& out) -> bool
{
  if (caps == nullptr || gst_caps_is_empty(caps) || gst_caps_get_size(caps) == 0) {
    return false;
  }
  const char* name =
      gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (g_strcmp0(name, "video/x-h264") == 0) {
    out = es_frame_codec::h264;
    return true;
  }
  if (g_strcmp0(name, "video/x-h265") == 0) {
    out = es_frame_codec::h265;
    return true;
  }
  if (g_strcmp0(name, "video/x-av1") == 0) {
    out = es_frame_codec::av1;
    return true;
  }
  return false;
}

auto caps_framing(GstCaps* caps) -> es_nal_framing
{
  if (caps == nullptr || gst_caps_get_size(caps) == 0) {
    return es_nal_framing::annexb;
  }
  const char* format = gst_structure_get_string(
      gst_caps_get_structure(caps, 0), "stream-format");
  if (format != nullptr
      && (std::strcmp(format, "avc") == 0 || std::strcmp(format, "avc3") == 0)) {
    return es_nal_framing::length_prefixed;
  }
  return es_nal_framing::annexb;
}
#endif  // OBR_HAVE_GSTREAMER
}  // namespace

auto au_starts_keyframe(es_frame_codec codec, es_nal_framing framing,
                        const std::uint8_t* data, std::size_t len) -> bool
{
  if (data == nullptr || len == 0) {
    return false;
  }
  if (codec == es_frame_codec::av1) {
    return obu_starts_keyframe(data, len);
  }

  // For h264/h265 every NAL of the unit is examined: the picture NAL is rarely
  // the first one (an AUD, an SEI or SPS/PPS comes first), and a check on the
  // literal first NAL would miss every keyframe in an x264enc stream.
  auto check = [codec](std::uint8_t first_byte) -> bool {
    if (codec == es_frame_codec::h264) {
      const int type = first_byte & 0x1F;
      return type == 5 || type == 7;  // IDR slice, SPS
    }
    const int type = (first_byte >> 1) & 0x3F;
    // IRAP pictures (BLA_W_LP..CRA), VPS, SPS.
    return (type >= 16 && type <= 23) || type == 32 || type == 33;
  };

  if (framing == es_nal_framing::length_prefixed) {
    return scan_length_prefixed(
        data, len,
        [&check](std::uint8_t first, const std::uint8_t*, std::size_t) {
          return check(first);
        });
  }
  return scan_annexb(
      data, len,
      [&check](std::uint8_t first, const std::uint8_t*, std::size_t) {
        return check(first);
      });
}

auto first_picture_nal_type(es_frame_codec codec, es_nal_framing framing,
                            const std::uint8_t* data, std::size_t len) -> int
{
  if (data == nullptr || len == 0 || codec == es_frame_codec::av1) {
    return -1;
  }
  int picture_type = -1;
  auto check = [codec, &picture_type](std::uint8_t first_byte) -> bool {
    const int type = codec == es_frame_codec::h264 ? (first_byte & 0x1F)
                                                   : ((first_byte >> 1) & 0x3F);
    // h264 slices are 1 (non-IDR) and 5 (IDR); h265 VCL types are 0..31.
    const bool is_picture = codec == es_frame_codec::h264
                                ? (type == 1 || type == 5)
                                : (type <= 31);
    if (is_picture) {
      picture_type = type;
      return true;
    }
    return false;
  };
  if (framing == es_nal_framing::length_prefixed) {
    scan_length_prefixed(data, len,
                         [&check](std::uint8_t first, const std::uint8_t*,
                                  std::size_t) { return check(first); });
  } else {
    scan_annexb(data, len,
                [&check](std::uint8_t first, const std::uint8_t*,
                         std::size_t) { return check(first); });
  }
  return picture_type;
}

#ifdef OBR_HAVE_GSTREAMER

stream_switch::stream_switch(log_fn log)
    : m_log(std::move(log))
{
  if (!m_log) {
    m_log = default_log;
  }
}

stream_switch::~stream_switch()
{
  detach();
}

auto stream_switch::note(const std::string& message) const -> void
{
  if (m_log) {
    m_log(message);
  }
}

auto stream_switch::attach(GstElement* pipeline, const switch_config& cfg)
    -> bool
{
  if (pipeline == nullptr || !GST_IS_BIN(pipeline)) {
    note("stream_switch: attach needs the pipeline the selector lives in");
    return false;
  }

  std::lock_guard<std::mutex> guard(m_mutex);
  if (m_sel != nullptr) {
    note("stream_switch: already attached");
    return false;
  }

  GstElement* sel = gst_bin_get_by_name(GST_BIN(pipeline), "sel");
  if (sel == nullptr) {
    note("stream_switch: no input-selector named 'sel' in the pipeline");
    return false;
  }
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(sel), "active-pad")
      == nullptr) {
    note("stream_switch: 'sel' is not an input-selector (no active-pad)");
    gst_object_unref(sel);
    return false;
  }

  GstPad* live = find_pad(sel, "sink_0");
  GstPad* slate = find_pad(sel, "sink_1");
  GstPad* src = gst_element_get_static_pad(sel, "src");
  auto fail = [&](const std::string& why) -> bool {
    note(why);
    if (live != nullptr) {
      gst_object_unref(live);
    }
    if (slate != nullptr) {
      gst_object_unref(slate);
    }
    if (src != nullptr) {
      gst_object_unref(src);
    }
    gst_object_unref(sel);
    return false;
  };
  if (live == nullptr || slate == nullptr) {
    return fail("stream_switch: 'sel' has no sink_0/sink_1 request pads");
  }
  if (src == nullptr) {
    return fail("stream_switch: 'sel' has no src pad");
  }

  // Caps equality is the invariant: the slate exists to look like the live
  // feed, and a caps change at the selector is a renegotiation the muxer turns
  // into a reconnect and a split video. Refuse rather than switch into it.
  GstCaps* live_caps = branch_caps(live, cfg.live_caps);
  GstCaps* slate_caps = branch_caps(slate, cfg.slate_caps);
  if (live_caps == nullptr || slate_caps == nullptr) {
    if (live_caps != nullptr) {
      gst_caps_unref(live_caps);
    }
    if (slate_caps != nullptr) {
      gst_caps_unref(slate_caps);
    }
    return fail(
        "stream_switch: REFUSING to attach: branch caps unknown (set "
        "live_caps/slate_caps or attach after the branches negotiated)");
  }

  m_caps_ok.store(gst_caps_is_equal(live_caps, slate_caps),
                  std::memory_order_release);
  if (!m_caps_ok.load(std::memory_order_acquire)) {
    gchar* live_str = gst_caps_to_string(live_caps);
    gchar* slate_str = gst_caps_to_string(slate_caps);
    const std::string message = std::format(
        "stream_switch: REFUSING to attach: live and slate caps differ\n"
        "  live:  {}\n  slate: {}",
        live_str != nullptr ? live_str : "?",
        slate_str != nullptr ? slate_str : "?");
    g_free(live_str);
    g_free(slate_str);
    gst_caps_unref(live_caps);
    gst_caps_unref(slate_caps);
    return fail(message);
  }

  es_frame_codec codec = es_frame_codec::h264;
  const bool have_codec = caps_codec(live_caps, codec);
  if (!cfg.audio && cfg.wait_for_keyframe && !have_codec) {
    gst_caps_unref(live_caps);
    gst_caps_unref(slate_caps);
    return fail(
        "stream_switch: REFUSING to attach: keyframe gating needs h264/h265/av1 "
        "video caps");
  }
  if (have_codec) {
    m_codec = codec;
    m_framing = caps_framing(live_caps);
  }
  gst_caps_unref(live_caps);
  gst_caps_unref(slate_caps);

  m_cfg = cfg;
  m_sel = sel;
  m_live_pad = live;
  m_slate_pad = slate;
  m_src_pad = src;

  m_live_probe = gst_pad_add_probe(live, GST_PAD_PROBE_TYPE_BUFFER,
                                   &stream_switch::live_probe_cb, this, nullptr);
  m_src_probe = gst_pad_add_probe(
      src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &stream_switch::src_probe_cb,
      this, nullptr);
  {
    // Baseline for caps-change counting: whatever the selector has already
    // negotiated is not a change.
    GstCaps* current = gst_pad_get_current_caps(src);
    if (current != nullptr) {
      std::lock_guard<std::mutex> caps_guard(m_caps_mutex);
      gst_caps_replace(&m_last_caps, current);
      gst_caps_unref(current);
    }
  }

  m_attached.store(true, std::memory_order_release);
  const transition first = apply_desired_locked();
  note("stream_switch: attached (branch caps equal)");
  if (first == transition::to_slate) {
    note("standby switch: slate");
  } else if (first == transition::to_live) {
    note("standby switch: live (immediate)");
  } else if (first == transition::waiting) {
    note("standby switch: waiting for a live keyframe");
  }
  return true;
}

auto stream_switch::detach() -> void
{
  // Probes are removed outside the mutex: gst_pad_remove_probe waits for an
  // in-flight probe, and a probe that is waiting for the mutex while detach
  // holds it would deadlock.
  GstPad* src_pad = nullptr;
  GstPad* live_pad = nullptr;
  guint src_probe = 0;
  guint live_probe = 0;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_src_pad != nullptr) {
      src_pad = GST_PAD(gst_object_ref(m_src_pad));
      src_probe = m_src_probe;
      m_src_probe = 0;
    }
    if (m_live_pad != nullptr) {
      live_pad = GST_PAD(gst_object_ref(m_live_pad));
      live_probe = m_live_probe;
      m_live_probe = 0;
    }
    m_attached.store(false, std::memory_order_release);
  }
  if (src_pad != nullptr) {
    if (src_probe != 0) {
      gst_pad_remove_probe(src_pad, src_probe);
    }
    gst_object_unref(src_pad);
  }
  if (live_pad != nullptr) {
    if (live_probe != 0) {
      gst_pad_remove_probe(live_pad, live_probe);
    }
    gst_object_unref(live_pad);
  }
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_src_pad != nullptr) {
      gst_object_unref(m_src_pad);
      m_src_pad = nullptr;
    }
    if (m_slate_pad != nullptr) {
      gst_object_unref(m_slate_pad);
      m_slate_pad = nullptr;
    }
    if (m_live_pad != nullptr) {
      gst_object_unref(m_live_pad);
      m_live_pad = nullptr;
    }
    if (m_sel != nullptr) {
      gst_object_unref(m_sel);
      m_sel = nullptr;
    }
    m_pending_live.store(false, std::memory_order_release);
    m_slate_active.store(false, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> caps_guard(m_caps_mutex);
    gst_caps_replace(&m_last_caps, nullptr);
  }
}

auto stream_switch::apply_desired_locked() -> transition
{
  if (m_sel == nullptr || m_live_pad == nullptr || m_slate_pad == nullptr) {
    return transition::none;  // recorded; applied at attach
  }
  if (m_desired_slate) {
    m_pending_live.store(false, std::memory_order_release);
    if (m_slate_active.load(std::memory_order_acquire)) {
      return transition::none;
    }
    g_object_set(m_sel, "active-pad", m_slate_pad, nullptr);
    m_slate_active.store(true, std::memory_order_release);
    return transition::to_slate;
  }
  if (!m_cfg.wait_for_keyframe || m_cfg.audio) {
    // Audio frames are independent and every all-transcode session re-encodes
    // the frame it receives, so neither waits: flip at the buffer boundary.
    m_pending_live.store(false, std::memory_order_release);
    if (!m_slate_active.load(std::memory_order_acquire)) {
      return transition::none;
    }
    g_object_set(m_sel, "active-pad", m_live_pad, nullptr);
    m_slate_active.store(false, std::memory_order_release);
    return transition::to_live;
  }
  // Keyframe-gated: ask for live, but leave the slate where it is until the
  // live branch presents a keyframe.
  if (m_slate_active.load(std::memory_order_acquire)) {
    if (m_pending_live.load(std::memory_order_acquire)) {
      return transition::none;
    }
    m_pending_live.store(true, std::memory_order_release);
    return transition::waiting;
  }
  m_pending_live.store(false, std::memory_order_release);
  return transition::none;
}

auto stream_switch::set_slate(bool slate) -> void
{
  transition result = transition::none;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_desired_slate = slate;
    result = apply_desired_locked();
  }
  // Logged outside the lock: a caller's log callback must never be able to
  // deadlock the streaming path.
  if (result == transition::to_slate) {
    note("standby switch: slate");
  } else if (result == transition::to_live) {
    note("standby switch: live (immediate)");
  } else if (result == transition::waiting) {
    note("standby switch: waiting for a live keyframe");
  }
}

auto stream_switch::buffer_starts_keyframe(GstBuffer* buf) const -> bool
{
  GstMapInfo map{};
  if (buf == nullptr || !gst_buffer_map(buf, &map, GST_MAP_READ)) {
    return false;
  }
  const bool key = au_starts_keyframe(
      m_codec, m_framing, static_cast<const std::uint8_t*>(map.data),
      static_cast<std::size_t>(map.size));
  gst_buffer_unmap(buf, &map);
  return key;
}

auto stream_switch::maybe_switch_to_live(GstBuffer* buf) -> void
{
  if (!m_pending_live.load(std::memory_order_acquire)) {
    return;
  }
  if (buf == nullptr || m_cfg.audio || !m_cfg.wait_for_keyframe) {
    return;
  }
  // Expensive part (parsing the AU) outside the mutex.
  if (!buffer_starts_keyframe(buf)) {
    return;
  }
  bool switched = false;
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    // Re-check under the lock: a set_slate(true) may have won the race since
    // the flag was read above.
    if (!m_pending_live.load(std::memory_order_acquire) || m_sel == nullptr) {
      return;
    }
    // The buffer is AU aligned (alignment=au) and this probe runs before the
    // selector's chain function, so activating the live pad here makes THIS
    // keyframe the first live buffer downstream. Never mid-AU.
    g_object_set(m_sel, "active-pad", m_live_pad, nullptr);
    m_pending_live.store(false, std::memory_order_release);
    m_slate_active.store(false, std::memory_order_release);
    switched = true;
  }
  if (switched) {
    note("standby switch: live (keyframe)");
  }
}

auto stream_switch::note_caps(GstCaps* caps) -> void
{
  std::lock_guard<std::mutex> guard(m_caps_mutex);
  if (caps == nullptr) {
    return;
  }
  if (m_last_caps == nullptr) {
    // The first caps event establishes the baseline; it is not a change.
    gst_caps_replace(&m_last_caps, caps);
    return;
  }
  if (gst_caps_is_equal(m_last_caps, caps)) {
    return;  // the selector re-sending the active pad's sticky caps
  }
  gst_caps_replace(&m_last_caps, caps);
  m_caps_changes.fetch_add(1, std::memory_order_relaxed);
}

auto stream_switch::live_probe_cb(GstPad* /*pad*/, GstPadProbeInfo* info,
                                  gpointer user) -> GstPadProbeReturn
{
  auto* self = static_cast<stream_switch*>(user);
  self->maybe_switch_to_live(GST_PAD_PROBE_INFO_BUFFER(info));
  return GST_PAD_PROBE_OK;
}

auto stream_switch::src_probe_cb(GstPad* /*pad*/, GstPadProbeInfo* info,
                                 gpointer user) -> GstPadProbeReturn
{
  auto* self = static_cast<stream_switch*>(user);
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (event != nullptr && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    self->note_caps(caps);
  }
  return GST_PAD_PROBE_OK;
}

#endif  // OBR_HAVE_GSTREAMER

}  // namespace standby
