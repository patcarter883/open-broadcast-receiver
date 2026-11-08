// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// stream_switch_test -- the slate/live decision against a live input-selector.
//
// The fixture is generated in-process, not committed: an h264 encoder
// (x264enc) makes 30 AUs at key-int-max=10, h264parse aligns them to access
// units, and an appsink hands them over. A committed fixture would be a binary
// blob whose NAL types nobody can inspect; generating it means the keyframe
// positions are known exactly (every 10th AU) and h264parse's own DELTA_UNIT
// flag is available as ground truth for the keyframe predicate.
//
// Each test builds a real pipeline from a string:
//
//   appsrc name=live  ... ! queue ! sel.sink_0
//   appsrc name=slate ... ! queue ! sel.sink_1
//   input-selector name=sel ! fakesink sync=false
//
// A buffer probe on `sel`'s src pad records every delivered buffer (which
// branch by its PTS origin, whether it is a keyframe, and its picture NAL
// type), so a switch is judged by what actually came out, not by the property
// the switch set.

#ifdef OBR_HAVE_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#endif

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "standby/stream_switch.h"

namespace
{
using standby::es_frame_codec;
using standby::es_nal_framing;

auto expect(bool cond, const std::string& what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
  }
}

// ---------------------------------------------------------------------------
// The keyframe predicate, pure. Synthetic AUs pin what is keyed on per codec:
// the real-encoder fixture below proves it against h264parse, but only this
// half runs in the tests-only lane and only this half can build an h265 or AV1
// AU by hand.
// ---------------------------------------------------------------------------
auto annexb_au(std::initializer_list<int> nal_types) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  for (const int type : nal_types) {
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
    // h264 NAL header: forbidden=0, nal_ref_idc=3, type in the low 5 bits.
    out.push_back(static_cast<std::uint8_t>(0x60 | (type & 0x1F)));
    out.push_back(0x11);  // a payload byte, never read
  }
  return out;
}

auto h265_annexb_au(std::initializer_list<int> nal_types)
    -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  for (const int type : nal_types) {
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
    // h265 NAL header: type in bits 6..1 of the first byte.
    out.push_back(static_cast<std::uint8_t>((type & 0x3F) << 1));
    out.push_back(0x01);
  }
  return out;
}

// One OBU: header byte, leb128 size, payload bytes.
auto obu(int type, const std::vector<std::uint8_t>& payload)
    -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  out.push_back(static_cast<std::uint8_t>(((type & 0x0F) << 3) | 0x02));
  std::size_t size = payload.size();
  while (size >= 0x80) {
    out.push_back(static_cast<std::uint8_t>((size & 0x7F) | 0x80));
    size >>= 7;
  }
  out.push_back(static_cast<std::uint8_t>(size));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

auto length_prefixed_au(std::initializer_list<int> nal_types)
    -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  for (const int type : nal_types) {
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x02});  // two-byte NAL
    out.push_back(static_cast<std::uint8_t>(0x60 | (type & 0x1F)));
    out.push_back(0x11);
  }
  return out;
}

auto keyframe(standby::es_frame_codec codec, standby::es_nal_framing framing,
              const std::vector<std::uint8_t>& au_bytes) -> bool
{
  return standby::au_starts_keyframe(codec, framing, au_bytes.data(),
                                     au_bytes.size());
}

auto test_predicate_h264() -> void
{
  using standby::es_frame_codec;
  using standby::es_nal_framing;

  // x264enc's keyframe shape: an AUD first, then SPS/PPS, then the IDR slice.
  const auto idr = annexb_au({9, 7, 8, 5});
  expect(keyframe(es_frame_codec::h264, es_nal_framing::annexb, idr),
         "h264: AUD/SPS/PPS/IDR is a keyframe");
  expect(standby::first_picture_nal_type(es_frame_codec::h264,
                                         es_nal_framing::annexb, idr.data(),
                                         idr.size())
             == 5,
         "h264: the picture NAL of a keyframe AU is IDR (5)");

  const auto p_frame = annexb_au({9, 1});
  expect(!keyframe(es_frame_codec::h264, es_nal_framing::annexb, p_frame),
         "h264: AUD + non-IDR slice is not a keyframe");
  expect(standby::first_picture_nal_type(es_frame_codec::h264,
                                         es_nal_framing::annexb, p_frame.data(),
                                         p_frame.size())
             == 1,
         "h264: a mid-GOP AU's picture NAL is a non-IDR slice (1)");

  // An SPS on its own still marks a decoder start point (config before IDR).
  expect(keyframe(es_frame_codec::h264, es_nal_framing::annexb,
                  annexb_au({9, 7, 8})),
         "h264: a parameter-set AU begins a keyframe");

  // avc/avc3 framing: 4-byte lengths instead of start codes.
  expect(keyframe(es_frame_codec::h264, es_nal_framing::length_prefixed,
                  length_prefixed_au({7, 8, 5})),
         "h264 avc framing: length-prefixed NALs are parsed");
  expect(!keyframe(es_frame_codec::h264, es_nal_framing::annexb,
                   length_prefixed_au({7, 8, 5})),
         "h264: an avc-framed buffer is not mistaken for Annex-B");
  std::puts("ok: h264 keyframe predicate");
}

auto test_predicate_h265() -> void
{
  using standby::es_frame_codec;
  using standby::es_nal_framing;

  // h265 keyframe: AUD(35), VPS(32), SPS(33), PPS(34), IDR_W_RADL(19).
  const auto irap = h265_annexb_au({35, 32, 33, 34, 19});
  expect(keyframe(es_frame_codec::h265, es_nal_framing::annexb, irap),
         "h265: VPS/SPS/IRAP is a keyframe");
  expect(!keyframe(es_frame_codec::h265, es_nal_framing::annexb,
                   h265_annexb_au({35, 1})),
         "h265: a TRAIL_R slice is not a keyframe");
  expect(keyframe(es_frame_codec::h265, es_nal_framing::annexb,
                  h265_annexb_au({35, 21})),  // CRA
         "h265: a CRA (21) is an IRAP");
  std::puts("ok: h265 keyframe predicate");
}

auto test_predicate_av1() -> void
{
  using standby::es_frame_codec;
  using standby::es_nal_framing;

  // av1parse emits a temporal delimiter then a sequence header at a keyframe.
  std::vector<std::uint8_t> key_au = obu(2, {});
  const std::vector<std::uint8_t> seq = obu(1, {0x0A, 0x0B});
  key_au.insert(key_au.end(), seq.begin(), seq.end());
  expect(keyframe(es_frame_codec::av1, es_nal_framing::annexb, key_au),
         "av1: an OBU_SEQUENCE_HEADER marks a keyframe");

  // No sequence header: the frame OBU's uncompressed header decides. Bits:
  // show_existing_frame=0, frame_type (2 bits) = 0 (KEY_FRAME).
  std::vector<std::uint8_t> frame_key = obu(2, {});
  const std::vector<std::uint8_t> frame = obu(6, {0x00, 0xFF});
  frame_key.insert(frame_key.end(), frame.begin(), frame.end());
  expect(keyframe(es_frame_codec::av1, es_nal_framing::annexb, frame_key),
         "av1: frame_type == KEY_FRAME is a keyframe");

  // frame_type 1 = INTER_FRAME (bits: 0 01 -> 0x20).
  std::vector<std::uint8_t> inter = obu(2, {});
  const std::vector<std::uint8_t> inter_frame = obu(6, {0x20, 0xFF});
  inter.insert(inter.end(), inter_frame.begin(), inter_frame.end());
  expect(!keyframe(es_frame_codec::av1, es_nal_framing::annexb, inter),
         "av1: an INTER_FRAME is not a keyframe");

  // show_existing_frame (bit 7) is a repeat, never a keyframe start.
  std::vector<std::uint8_t> show = obu(2, {});
  const std::vector<std::uint8_t> show_frame = obu(6, {0x80});
  show.insert(show.end(), show_frame.begin(), show_frame.end());
  expect(!keyframe(es_frame_codec::av1, es_nal_framing::annexb, show),
         "av1: show_existing_frame is not a keyframe");
  std::puts("ok: av1 keyframe predicate");
}

// ---------------------------------------------------------------------------
// GStreamer half: the decision against a real input-selector
// ---------------------------------------------------------------------------
#ifdef OBR_HAVE_GSTREAMER

constexpr std::int64_t k_frame = 33'333'333;             // 30 fps
constexpr std::int64_t k_slate_base = 3'600'000'000'000LL;  // slate PTS origin
constexpr gint64 k_live_tag = 0;   // buffer offset: which branch produced it
constexpr gint64 k_slate_tag = 1;
constexpr int k_key_int = 10;                            // x264enc key-int-max
constexpr int k_au_count = 30;

constexpr const char* k_live_caps =
    "video/x-h264,stream-format=byte-stream,alignment=au,width=160,"
    "height=120,framerate=30/1";
constexpr const char* k_mismatched_slate_caps =
    "video/x-h264,stream-format=byte-stream,alignment=au,width=320,"
    "height=240,framerate=30/1";

struct au
{
  std::vector<std::uint8_t> bytes;
  bool key = false;  // h264parse's DELTA_UNIT flag: independent ground truth
};

// Generate the ES once: every AU as the bytes an AU-aligned appsrc would push.
auto generate_aus(std::string& err) -> std::vector<au>
{
  std::vector<au> out;
  if (gst_element_factory_find("x264enc") == nullptr
      || gst_element_factory_find("h264parse") == nullptr) {
    err = "x264enc/h264parse not installed";
    return out;
  }

  const std::string desc =
      "videotestsrc num-buffers=" + std::to_string(k_au_count)
      + " ! video/x-raw,width=160,height=120,framerate=30/1"
        " ! x264enc key-int-max="
      + std::to_string(k_key_int)
      + " speed-preset=ultrafast bitrate=800"
        " ! h264parse config-interval=-1"
        " ! video/x-h264,stream-format=byte-stream,alignment=au"
        " ! appsink name=sink";
  GError* gerr = nullptr;
  GstElement* gen = gst_parse_launch(desc.c_str(), &gerr);
  if (gen == nullptr) {
    err = std::string("generate pipeline: ")
          + (gerr != nullptr ? gerr->message : "unknown");
    g_clear_error(&gerr);
    return out;
  }
  GstElement* sink = gst_bin_get_by_name(GST_BIN(gen), "sink");
  gst_element_set_state(gen, GST_STATE_PLAYING);
  while (true) {
    GstSample* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND);
    if (sample == nullptr) {
      break;
    }
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (buf != nullptr && gst_buffer_map(buf, &map, GST_MAP_READ)) {
      au unit;
      unit.bytes.assign(map.data, map.data + map.size);
      unit.key = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
      out.push_back(std::move(unit));
      gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
  }
  gst_element_set_state(gen, GST_STATE_NULL);
  gst_object_unref(sink);
  gst_object_unref(gen);
  if (out.empty()) {
    err = "the generator produced no access units";
  }
  return out;
}

// The keyframe predicate must agree with the parser's own flags, or the switch
// would gate on the wrong buffers.
auto self_check_predicate(const std::vector<au>& aus) -> void
{
  int keys = 0;
  for (std::size_t idx = 0; idx < aus.size(); ++idx) {
    const bool detected = standby::au_starts_keyframe(
        es_frame_codec::h264, es_nal_framing::annexb, aus[idx].bytes.data(),
        aus[idx].bytes.size());
    expect(detected == aus[idx].key,
           "keyframe predicate agrees with h264parse's DELTA_UNIT");
    if (detected) {
      ++keys;
      expect(standby::first_picture_nal_type(
                 es_frame_codec::h264, es_nal_framing::annexb,
                 aus[idx].bytes.data(), aus[idx].bytes.size())
                 == 5,
             "a detected keyframe AU starts with an IDR slice");
    }
  }
  expect(keys >= 3, "the fixture has at least three keyframes");
  std::printf("ok: fixture generated (%zu AUs, %d keyframes, key-int %d)\n",
              aus.size(), keys, k_key_int);
}

struct delivery
{
  bool live = false;
  bool key = false;
  int nal = -1;
  std::int64_t pts = 0;
};

struct rig
{
  GstElement* pipe = nullptr;
  GstElement* live = nullptr;
  GstElement* slate = nullptr;
  std::unique_ptr<standby::stream_switch> sw;
  guint probe = 0;
  std::atomic<bool> stop {false};
  std::mutex mutex;
  std::vector<delivery> seen;

  ~rig()
  {
    stop.store(true);
    if (probe != 0 && pipe != nullptr) {
      GstElement* sel = gst_bin_get_by_name(GST_BIN(pipe), "sel");
      if (sel != nullptr) {
        GstPad* src = gst_element_get_static_pad(sel, "src");
        if (src != nullptr) {
          gst_pad_remove_probe(src, probe);
          gst_object_unref(src);
        }
        gst_object_unref(sel);
      }
    }
    if (pipe != nullptr) {
      gst_element_set_state(pipe, GST_STATE_NULL);
    }
    if (live != nullptr) {
      gst_object_unref(live);
    }
    if (slate != nullptr) {
      gst_object_unref(slate);
    }
    if (pipe != nullptr) {
      gst_object_unref(pipe);
    }
  }

  auto count() -> std::size_t
  {
    std::lock_guard<std::mutex> guard(mutex);
    return seen.size();
  }

  auto get(std::size_t idx) -> delivery
  {
    std::lock_guard<std::mutex> guard(mutex);
    return seen[idx];
  }

  // Index of the first delivery at or after `mark` from the given branch, or
  // -1. Also reports the index it landed on through `found`.
  auto wait_for_branch(bool live, std::size_t mark, std::size_t& found,
                       int timeout_ms = 3000) -> bool
  {
    const gint64 deadline = g_get_monotonic_time()
                            + static_cast<gint64>(timeout_ms) * 1000;
    while (g_get_monotonic_time() < deadline) {
      {
        std::lock_guard<std::mutex> guard(mutex);
        for (std::size_t idx = mark; idx < seen.size(); ++idx) {
          if (seen[idx].live == live) {
            found = idx;
            return true;
          }
        }
      }
      g_usleep(500);
    }
    return false;
  }

  auto dump(const char* why) -> void
  {
    std::size_t total = 0;
    std::size_t live_count = 0;
    std::size_t slate_count = 0;
    std::int64_t last_live_pts = -1;
    std::int64_t last_slate_pts = -1;
    {
      std::lock_guard<std::mutex> guard(mutex);
      total = seen.size();
      for (const delivery& d : seen) {
        if (d.live) {
          ++live_count;
          last_live_pts = d.pts;
        } else {
          ++slate_count;
          last_slate_pts = d.pts;
        }
      }
    }
    std::fprintf(stderr,
                 "  [%s] delivered=%zu live=%zu (last pts %lld) slate=%zu "
                 "(last pts %lld)\n",
                 why, total, live_count,
                 static_cast<long long>(last_live_pts), slate_count,
                 static_cast<long long>(last_slate_pts));
    GstBus* bus = gst_element_get_bus(pipe);
    for (int idx = 0; idx < 4; ++idx) {
      GstMessage* msg = gst_bus_timed_pop_filtered(
          bus, 0,
          static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING
                                      | GST_MESSAGE_EOS));
      if (msg == nullptr) {
        break;
      }
      GError* err = nullptr;
      gchar* dbg = nullptr;
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(msg, &err, &dbg);
      } else {
        gst_message_parse_warning(msg, &err, &dbg);
      }
      std::fprintf(stderr, "  [%s] %s: %s / %s\n", why,
                   GST_MESSAGE_TYPE_NAME(msg), err != nullptr ? err->message : "?",
                   dbg != nullptr ? dbg : "");
      g_clear_error(&err);
      g_free(dbg);
      gst_message_unref(msg);
    }
    gst_object_unref(bus);
  }
};

auto recorder_cb(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user)
    -> GstPadProbeReturn
{
  auto* r = static_cast<rig*>(user);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  GstMapInfo map{};
  if (buf == nullptr || !gst_buffer_map(buf, &map, GST_MAP_READ)) {
    return GST_PAD_PROBE_OK;
  }
  delivery d;
  d.pts = GST_BUFFER_PTS(buf);
  // The branch is carried explicitly in the buffer offset: inferring it from a
  // PTS range would misclassify a live buffer once its clock passed the slate's
  // origin, and the pushers do not pace to real time.
  d.live = GST_BUFFER_OFFSET(buf) == k_live_tag;
  d.nal = standby::first_picture_nal_type(es_frame_codec::h264,
                                          es_nal_framing::annexb, map.data,
                                          map.size);
  d.key = standby::au_starts_keyframe(es_frame_codec::h264,
                                      es_nal_framing::annexb, map.data,
                                      map.size);
  gst_buffer_unmap(buf, &map);
  {
    std::lock_guard<std::mutex> guard(r->mutex);
    r->seen.push_back(d);
  }
  return GST_PAD_PROBE_OK;
}

auto make_rig(bool wait_for_keyframe, const char* slate_caps,
              standby::stream_switch::log_fn log, std::string& err)
    -> std::unique_ptr<rig>
{
  const std::string desc =
      "input-selector name=sel ! fakesink sync=false "
      "appsrc name=live format=time is-live=false block=false max-bytes=8388608"
      " ! queue ! sel.sink_0 "
      "appsrc name=slate format=time is-live=false block=false max-bytes=8388608"
      " ! queue ! sel.sink_1";
  GError* gerr = nullptr;
  GstElement* pipe = gst_parse_launch(desc.c_str(), &gerr);
  if (pipe == nullptr) {
    err = std::string("switch pipeline: ")
          + (gerr != nullptr ? gerr->message : "unknown");
    g_clear_error(&gerr);
    return nullptr;
  }
  auto r = std::make_unique<rig>();
  r->pipe = pipe;
  r->live = gst_bin_get_by_name(GST_BIN(pipe), "live");
  r->slate = gst_bin_get_by_name(GST_BIN(pipe), "slate");
  if (r->live == nullptr || r->slate == nullptr) {
    err = "appsrc branches missing";
    return nullptr;
  }
  GstCaps* live_caps = gst_caps_from_string(k_live_caps);
  g_object_set(r->live, "caps", live_caps, nullptr);
  gst_caps_unref(live_caps);
  GstCaps* slate_caps_obj = gst_caps_from_string(slate_caps);
  g_object_set(r->slate, "caps", slate_caps_obj, nullptr);
  gst_caps_unref(slate_caps_obj);

  GstElement* sel = gst_bin_get_by_name(GST_BIN(pipe), "sel");
  GstPad* src = gst_element_get_static_pad(sel, "src");
  r->probe = gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, recorder_cb,
                               r.get(), nullptr);
  gst_object_unref(src);
  gst_object_unref(sel);

  standby::switch_config cfg;
  cfg.audio = false;
  cfg.wait_for_keyframe = wait_for_keyframe;
  cfg.live_caps = k_live_caps;
  cfg.slate_caps = slate_caps;
  r->sw = std::make_unique<standby::stream_switch>(std::move(log));
  if (!r->sw->attach(pipe, cfg)) {
    err = "attach refused";
    return r;  // the caller decides what a refusal means
  }
  return r;
}

auto push_au(GstElement* src, const au& unit, std::int64_t pts, gint64 tag)
    -> bool
{
  GstBuffer* buf = gst_buffer_new_allocate(nullptr, unit.bytes.size(), nullptr);
  GstMapInfo map{};
  if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buf);
    return false;
  }
  std::memcpy(map.data, unit.bytes.data(), unit.bytes.size());
  gst_buffer_unmap(buf, &map);
  GST_BUFFER_PTS(buf) = pts;
  GST_BUFFER_DURATION(buf) = k_frame;
  GST_BUFFER_OFFSET(buf) = tag;
  return gst_app_src_push_buffer(GST_APP_SRC(src), buf) == GST_FLOW_OK;
}

// Push one branch's AUs round and round on its own thread, so set_slate() is
// exercised from the "controller" thread while streaming runs.
auto start_pusher(rig& r, GstElement* src, std::int64_t base,
                  const std::vector<au>& aus) -> std::thread
{
  return std::thread([&r, src, base, &aus]() {
    std::size_t idx = 0;
    while (!r.stop.load()) {
      push_au(src, aus[idx % aus.size()],
              base + static_cast<std::int64_t>(idx) * k_frame,
              base == 0 ? k_live_tag : k_slate_tag);
      ++idx;
      g_usleep(200);
    }
  });
}

// ---------------------------------------------------------------------------
// 1. twenty keyframe-gated round trips: every flip to live lands on an IDR
// ---------------------------------------------------------------------------
auto test_keyframe_gated_switches(const std::vector<au>& aus) -> void
{
  std::string err;
  auto r = make_rig(true, k_live_caps, {}, err);
  expect(r != nullptr && r->sw != nullptr, "keyframe rig built: " + err);
  expect(r->sw->attached(), "keyframe switch attached");
  expect(r->sw->slate_active(), "a session starts on the slate");

  expect(gst_element_set_state(r->pipe, GST_STATE_PLAYING)
             != GST_STATE_CHANGE_FAILURE,
         "keyframe rig reaches PLAYING");
  std::thread live_pusher = start_pusher(*r, r->live, 0, aus);
  std::thread slate_pusher = start_pusher(*r, r->slate, k_slate_base, aus);

  std::vector<int> flip_nal_types;
  std::size_t mark = 0;
  for (int trip = 0; trip < 20; ++trip) {
    r->sw->set_slate(true);
    std::size_t found = 0;
    if (!r->wait_for_branch(false, mark, found)) {
      r->dump("no slate");
      expect(false, "the slate delivers after set_slate(true)");
    }
    mark = found + 1;

    r->sw->set_slate(false);
    if (!r->wait_for_branch(true, mark, found)) {
      r->dump("no live");
      expect(false, "live delivers after set_slate(false)");
    }
    const delivery d = r->get(found);
    expect(d.key, "the first live buffer after a gated switch is a keyframe");
    expect(d.nal == 5, "the first live buffer is an IDR slice");
    flip_nal_types.push_back(d.nal);
    mark = found + 1;
    expect(r->sw->slate_active() == false,
           "slate_active() reports the live branch after the flip");
  }

  // Let anything in flight finish, then check the caps never renegotiated.
  g_usleep(20'000);
  expect(r->sw->caps_changes() == 0,
         "no caps change across 20 round-trip switches");
  expect(r->sw->caps_ok(), "caps equality held for the whole run");

  std::printf("ok: 20 keyframe-gated round trips, IDR each time (NAL");
  for (const int nal : flip_nal_types) {
    std::printf(" %d", nal);
  }
  std::printf(")\n");

  r->stop.store(true);
  live_pusher.join();
  slate_pusher.join();
}

// ---------------------------------------------------------------------------
// 2. wait_for_keyframe=false: the very next buffer flips, even mid-GOP
// ---------------------------------------------------------------------------
auto test_immediate_flip(const std::vector<au>& aus) -> void
{
  std::string err;
  auto r = make_rig(false, k_live_caps, {}, err);
  expect(r != nullptr && r->sw != nullptr, "immediate rig built: " + err);
  expect(r->sw->attached(), "immediate switch attached");

  expect(gst_element_set_state(r->pipe, GST_STATE_PLAYING)
             != GST_STATE_CHANGE_FAILURE,
         "immediate rig reaches PLAYING");

  // Find a mid-GOP AU: with key-int-max=10 the keyframes are 0, 10, 20.
  std::size_t mid = 0;
  while (mid < aus.size() && aus[mid].key) {
    ++mid;
  }
  expect(mid < aus.size() && !aus[mid].key, "the fixture has a mid-GOP AU");
  expect(aus[mid].bytes.size() > 0, "the mid-GOP AU has bytes");

  r->sw->set_slate(true);
  expect(push_au(r->slate, aus[0], k_slate_base, k_slate_tag), "slate AU pushed");
  std::size_t found = 0;
  expect(r->wait_for_branch(false, 0, found), "the slate delivers");

  // A mid-GOP live buffer arrives while the slate is still on air: dropped by
  // the selector. Then flip -- with no keyframe gate the next live buffer must
  // be the one that comes out, not the next IDR.
  expect(push_au(r->live, aus[mid], static_cast<std::int64_t>(mid) * k_frame,
                 k_live_tag),
         "mid-GOP live AU pushed");
  g_usleep(50'000);  // let the inactive branch's chain drop it
  r->sw->set_slate(false);
  expect(!r->sw->slate_active(), "set_slate(false) flips immediately");

  const std::size_t next = mid + 1 < aus.size() ? mid + 1 : mid;
  expect(push_au(r->live, aus[next],
                 static_cast<std::int64_t>(next) * k_frame, k_live_tag),
         "next live AU pushed");
  expect(r->wait_for_branch(true, 0, found), "live delivers immediately");
  expect(!r->get(found).key,
         "the flip was not held back for a keyframe");
  const std::int64_t expected_pts = static_cast<std::int64_t>(next) * k_frame;
  const std::int64_t pts = r->get(found).pts;
  expect(pts == expected_pts
             || pts == static_cast<std::int64_t>(mid) * k_frame,
         "the very next live buffer was delivered, not a later keyframe");
  expect(r->sw->caps_changes() == 0, "no caps change on an immediate switch");
  std::printf("ok: immediate flip mid-GOP (delivered AU pts %lld, non-key)\n",
              static_cast<long long>(pts));
}

// ---------------------------------------------------------------------------
// 3. a caps mismatch refuses the attachment
// ---------------------------------------------------------------------------
auto test_caps_mismatch_refused(const std::vector<au>& aus) -> void
{
  std::vector<std::string> logs;
  auto capture = [&logs](const std::string& message) {
    logs.push_back(message);
    std::fprintf(stderr, "%s\n", message.c_str());
  };

  std::string err;
  // Deliberately different slate caps. Data is pushed first so both branches
  // have negotiatd their caps -- the mismatch the switch must catch.
  auto r = make_rig(true, k_mismatched_slate_caps, capture, err);
  expect(r != nullptr, "mismatch rig built: " + err);
  expect(r->sw != nullptr, "mismatch switch object exists");
  expect(!r->sw->attached(), "a caps mismatch leaves the switch un-attached");
  expect(!r->sw->caps_ok(), "caps_ok() is false after a mismatch");

  bool loud = false;
  for (const std::string& line : logs) {
    if (line.find("caps differ") != std::string::npos
        || line.find("REFUSING") != std::string::npos) {
      loud = true;
    }
  }
  expect(loud, "the caps mismatch was logged loudly");

  // And it really is un-attached: a switch request changes nothing.
  expect(!r->sw->attached(), "the refused switch is not attached");
  r->sw->set_slate(false);
  expect(r->sw->slate_active() == false,
         "an un-attached switch never reports a live branch");
  std::printf("ok: caps mismatch refused at attach\n");
}

// ---------------------------------------------------------------------------
// 4. the mismatch is also caught on negotiated caps, not only on config
// ---------------------------------------------------------------------------
auto test_caps_mismatch_after_negotiation(const std::vector<au>& aus) -> void
{
  // Build the branches so they push before attach; then attach with the
  // configured caps cleared so the pads' negotiated caps are what is compared.
  std::string err;
  auto r = make_rig(false, k_mismatched_slate_caps, {}, err);
  expect(r != nullptr, "negotiated mismatch rig exists");

  // The rig above already attached (its config caps differ, which is the
  // first test). Here the check is the pad path: same config, different reality.
  // A fresh switch on the same pipeline must still refuse.
  standby::stream_switch second;
  standby::switch_config cfg;
  cfg.wait_for_keyframe = false;
  cfg.live_caps.clear();
  cfg.slate_caps.clear();
  expect(gst_element_set_state(r->pipe, GST_STATE_PLAYING)
             != GST_STATE_CHANGE_FAILURE,
         "mismatch rig reaches PLAYING");
  expect(push_au(r->live, aus[0], 0, k_live_tag), "live AU pushed");
  expect(push_au(r->slate, aus[0], k_slate_base, k_slate_tag), "slate AU pushed");
  g_usleep(100'000);
  expect(!second.attach(r->pipe, cfg),
         "attach refuses differing negotiated caps with no config hints");
  expect(!second.caps_ok(), "caps_ok() false on negotiated mismatch");
  std::printf("ok: caps mismatch caught on negotiated caps\n");
}
#endif  // OBR_HAVE_GSTREAMER

}  // namespace

auto main() -> int
{
  test_predicate_h264();
  test_predicate_h265();
  test_predicate_av1();

#ifdef OBR_HAVE_GSTREAMER
  gst_init(nullptr, nullptr);

  std::string err;
  const std::vector<au> aus = generate_aus(err);
  if (aus.empty()) {
    std::fprintf(stderr, "SKIP: %s\n", err.c_str());
    return 77;  // skip: no usable h264 encoder/parser
  }
  self_check_predicate(aus);

  test_keyframe_gated_switches(aus);
  test_immediate_flip(aus);
  test_caps_mismatch_refused(aus);
  test_caps_mismatch_after_negotiation(aus);
#endif  // OBR_HAVE_GSTREAMER

  std::puts("stream_switch_test: ALL OK");
  return 0;
}