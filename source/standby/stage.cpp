// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// stage — one demux, two selectors, one timeline, and the ES rings (plan C4).
//
// The pipeline shape and the reasons for each part are documented in stage.h.
// This file is the assembly: build the string, route the demuxer's dynamic pads
// by caps, wrap each input-selector in its own bin (stream_switch looks a
// selector up by the fixed name "sel"), install the publishing probes, and run
// the thread that turns the session's TS ring into the demuxer's appsrc.

#include "standby/stage.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

#ifdef OBR_HAVE_GSTREAMER

#include <gst/app/gstappsrc.h>

#include "standby/es_looper.h"
#include "standby/render.h"

namespace standby
{
namespace
{
constexpr std::size_t k_read_chunk = 4096;  // bytes per ts_ring read
constexpr std::uint64_t k_appsrc_max_bytes = 4UL * 1024 * 1024;
constexpr std::chrono::milliseconds k_idle_sleep {10};
constexpr int k_feed_burst = 8;  // ring reads per loop, keeps stop() prompt

auto parse_codec(const std::string& name, es_frame_codec& out) -> bool
{
  if (name == "h264") {
    out = es_frame_codec::h264;
    return true;
  }
  if (name == "h265" || name == "hevc") {
    out = es_frame_codec::h265;
    return true;
  }
  if (name == "av1") {
    out = es_frame_codec::av1;
    return true;
  }
  return false;
}

auto codec_parser(es_frame_codec codec) -> const char*
{
  switch (codec) {
    case es_frame_codec::h264:
      return "h264parse";
    case es_frame_codec::h265:
      return "h265parse";
    case es_frame_codec::av1:
      return "av1parse";
  }
  return "h264parse";
}

// h264/h265 repeat SPS/PPS inside the GOP (config-interval=-1) so a decoder (or
// a copy destination that joins at the loop point) has the parameter sets without
// waiting for the next IDR. av1parse has no such property.
auto parser_props(es_frame_codec codec) -> const char*
{
  return codec == es_frame_codec::av1 ? "" : " config-interval=-1";
}

// The exact caps BOTH branches are pinned to -- identical by construction, and
// the same strings the renderer uses for the slate file, so a slate can only
// ever be cut into a stream it fits.
auto video_caps(es_frame_codec codec) -> std::string
{
  switch (codec) {
    case es_frame_codec::h264:
      return "video/x-h264,stream-format=byte-stream,alignment=au";
    case es_frame_codec::h265:
      return "video/x-h265,stream-format=byte-stream,alignment=au";
    case es_frame_codec::av1:
      return "video/x-av1,stream-format=obu-stream,alignment=frame";
  }
  return {};
}

// The caps an appsrc carrying raw ES must announce: the parser's input format
// (byte-stream for h264/h265, obu-stream for AV1). Alignment is left to the
// parser, which is why it is not pinned here.
auto slate_src_caps(es_frame_codec codec) -> const char*
{
  switch (codec) {
    case es_frame_codec::h264:
      return "video/x-h264,stream-format=byte-stream";
    case es_frame_codec::h265:
      return "video/x-h265,stream-format=byte-stream";
    case es_frame_codec::av1:
      return "video/x-av1,stream-format=obu-stream";
  }
  return "video/x-h264,stream-format=byte-stream";
}

auto build_description(es_frame_codec codec, bool slate) -> std::string
{
  const std::string caps = video_caps(codec);
  std::string d = std::format(
      // tsparse requires explicit caps on its source. The demux is deliberately
      // NOT linked to a branch here: its pads are dynamic and a pad-agnostic
      // `d.` link would attach whichever pad appeared first (the audio one) to
      // the video branch. link_demux_pad() routes by caps instead.
      "appsrc name=dsrc is-live=true do-timestamp=true format=time "
      "block=false max-bytes=4194304 "
      "caps=video/mpegts,systemstream=true "
      "! tsparse set-timestamps=true alignment=7 ! tsdemux name=d "
      "queue name=vq ! {}{} ! {} ! capsfilter name=vcap0 "
      "queue name=aq ! aacparse "
      "! audio/mpeg,mpegversion=4,stream-format=adts,parsed=true "
      "! capsfilter name=acap0 "
      "fakesink name=vsink sync=false async=false "
      "fakesink name=asink sync=false async=false",
      codec_parser(codec), parser_props(codec), caps);
  if (!slate) {
    return d;
  }
  // The slate: pre-encoded ES replayed by es_looper. Its appsrc announces the
  // parser's input format; the branch is pinned to the SAME caps as the live
  // one, because a caps difference at the selector is a muxer renegotiation.
  d += std::format(
      " appsrc name=svsrc format=time is-live=true block=false "
      "max-bytes=4194304 caps={} "
      "! {}{} ! {} ! capsfilter name=vcap1 "
      "appsrc name=sasrc format=time is-live=true block=false "
      "max-bytes=4194304 "
      "caps=audio/mpeg,mpegversion=4,stream-format=adts,parsed=true "
      "! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts,parsed=true "
      "! capsfilter name=acap1",
      slate_src_caps(codec), codec_parser(codec), parser_props(codec), caps);
  return d;
}
}  // namespace

// adts_looper — the audio half of the slate.
//
// es_looper replays an elementary VIDEO stream (it reads NAL/OBU framing to
// split access units). A rendered slate's audio is a couple of seconds of ADTS
// AAC, which has no such framing, so it gets its own small loop here: split at
// ADTS frame boundaries (the header carries the frame length), push each frame
// into the slate appsrc, and count the PTS across passes exactly like the video
// looper, so the loop point never rewinds the timeline the follower probe sees.
class adts_looper
{
public:
  using log_fn = std::function<void(const std::string&)>;

  adts_looper(GstElement* appsrc, std::string path, log_fn log)
      : m_appsrc {appsrc}
      , m_path {std::move(path)}
      , m_log {std::move(log)}
  {
  }

  ~adts_looper()
  {
    stop();
  }

  adts_looper(const adts_looper&) = delete;
  auto operator=(const adts_looper&) -> adts_looper& = delete;

  auto start() -> bool
  {
    if (m_appsrc == nullptr) {
      note("no appsrc for the slate audio");
      return false;
    }
    std::ifstream in(m_path, std::ios::binary);
    if (!in) {
      note("cannot open the slate audio '" + m_path + "'");
      return false;
    }
    m_data.assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
    m_frames = adts_frames(m_data);
    if (m_frames.empty()) {
      note("no ADTS frames in the slate audio '" + m_path + "'");
      return false;
    }
    // 1024 samples per AAC frame at 48 kHz; the renderer produces exactly that.
    m_frame_ns = 1024LL * 1'000'000'000LL / 48000LL;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_running = true;
    }
    m_thread = std::thread(&adts_looper::run, this);
    return true;
  }

  auto stop() -> void
  {
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

private:
  auto note(const std::string& message) const -> void
  {
    if (m_log) {
      m_log("[stage] " + message);
    } else {
      std::fprintf(stderr, "[stage] %s\n", message.c_str());
    }
  }

  auto run() -> void
  {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t pushed = 0;
    while (true) {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_running) {
          return;
        }
      }
      const auto [offset, length] = m_frames[pushed % m_frames.size()];
      GstBuffer* buf = gst_buffer_new_allocate(nullptr, length, nullptr);
      gst_buffer_fill(buf, 0, m_data.data() + offset, length);
      GST_BUFFER_PTS(buf) = static_cast<GstClockTime>(pushed)
                            * static_cast<GstClockTime>(m_frame_ns);
      GST_BUFFER_DURATION(buf) = static_cast<GstClockTime>(m_frame_ns);
      // Takes ownership on every path.
      if (gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), buf) != GST_FLOW_OK) {
        note("the slate audio appsrc refused a buffer; ending the loop");
        return;
      }
      ++pushed;
      // Paced to real time: an unpaced loop floods the appsrc (block=false) and
      // the selector is then always looking at an old buffer.
      const auto due = start
                       + std::chrono::nanoseconds(static_cast<std::int64_t>(
                             pushed * static_cast<std::uint64_t>(m_frame_ns)));
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_until(lock, due, [this] { return !m_running; });
    }
  }

  static auto adts_frames(const std::vector<std::uint8_t>& data)
      -> std::vector<std::pair<std::size_t, std::size_t>>
  {
    std::vector<std::pair<std::size_t, std::size_t>> frames;
    std::size_t pos = 0;
    while (pos + 7 <= data.size()) {
      // 12-bit syncword; the third bit is ID, the fourth is the layer, both 0.
      if (data[pos] != 0xFF || (data[pos + 1] & 0xF0) != 0xF0) {
        ++pos;
        continue;
      }
      const std::size_t header = (data[pos + 1] & 0x01) != 0 ? 7 : 9;
      if (pos + header > data.size()) {
        break;
      }
      const std::size_t length =
          (static_cast<std::size_t>(data[pos + 3] & 0x03) << 11)
          | (static_cast<std::size_t>(data[pos + 4]) << 3)
          | (static_cast<std::size_t>(data[pos + 5] >> 5) & 0x07);
      if (length < header || pos + length > data.size()) {
        break;
      }
      frames.emplace_back(pos, length);
      pos += length;
    }
    return frames;
  }

  GstElement* m_appsrc;
  std::string m_path;
  log_fn m_log;
  std::vector<std::uint8_t> m_data;
  std::vector<std::pair<std::size_t, std::size_t>> m_frames;
  std::int64_t m_frame_ns = 0;
  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_running = false;
};

stage::stage(log_fn log)
    : m_log(std::move(log))
    , m_vswitch([this](const std::string& m) { note(m); })
    , m_aswitch([this](const std::string& m) { note(m); })
{
}

stage::~stage()
{
  stop();
}

auto stage::note(const std::string& message) const -> void
{
  if (m_log) {
    m_log("[stage] " + message);
    return;
  }
  std::fprintf(stderr, "[stage] %s\n", message.c_str());
}

auto stage::pipeline_description() const -> std::string
{
  std::lock_guard<std::mutex> guard(m_state_mutex);
  return m_description;
}

auto stage::stats() const -> stage_stats
{
  stage_stats s;
  s.slate_active = m_vswitch.attached() && m_vswitch.slate_active();
  s.switches = m_switches.load(std::memory_order_relaxed);
  s.rebuilds = m_rebuilds.load(std::memory_order_relaxed);
  s.caps_ok = m_caps_ok.load(std::memory_order_acquire);
  s.switch_degraded = m_vswitch.degraded() || m_aswitch.degraded();
  s.aus_in = m_aus_in.load(std::memory_order_relaxed);
  s.aus_out = m_aus_out.load(std::memory_order_relaxed);
  return s;
}

// ---------------------------------------------------------------------------
// build / destroy
// ---------------------------------------------------------------------------

namespace
{
// A selector in a bin of its own, with ghost pads named sink_0/sink_1/src, so
// both streams can carry an input-selector called "sel" in one pipeline.
auto make_selector(const char* bin_name, GstElement*& out_bin,
                   GstElement*& out_sel, GstPad*& out_sink0,
                   GstPad*& out_sink1, GstPad*& out_src) -> bool
{
  GstElement* bin = gst_bin_new(bin_name);
  GstElement* sel = gst_element_factory_make("input-selector", "sel");
  if (bin == nullptr || sel == nullptr) {
    if (bin != nullptr) {
      gst_object_unref(bin);
    }
    if (sel != nullptr) {
      gst_object_unref(sel);
    }
    return false;
  }
  gst_bin_add(GST_BIN(bin), sel);  // the bin owns it from here

  GstPad* sink0 = gst_element_request_pad_simple(sel, "sink_%u");
  GstPad* sink1 = gst_element_request_pad_simple(sel, "sink_%u");
  GstPad* src = gst_element_get_static_pad(sel, "src");
  if (sink0 == nullptr || sink1 == nullptr || src == nullptr) {
    if (sink0 != nullptr) {
      gst_object_unref(sink0);
    }
    if (sink1 != nullptr) {
      gst_object_unref(sink1);
    }
    if (src != nullptr) {
      gst_object_unref(src);
    }
    gst_object_unref(bin);
    return false;
  }

  for (auto [name, target] :
       {std::pair {"sink_0", sink0}, std::pair {"sink_1", sink1},
        std::pair {"src", src}}) {
    GstPad* ghost = gst_ghost_pad_new(name, target);
    if (ghost == nullptr || !gst_element_add_pad(bin, ghost)) {
      if (ghost != nullptr) {
        gst_object_unref(ghost);
      }
      gst_object_unref(sink0);
      gst_object_unref(sink1);
      gst_object_unref(src);
      gst_object_unref(bin);
      return false;
    }
  }

  // A ref of our own: the bin owns the element from gst_bin_add onwards, so
  // dropping the caller's reference without taking one kills a live child.
  out_bin = bin;
  out_sel = GST_ELEMENT(gst_object_ref(sel));
  out_sink0 = sink0;
  out_sink1 = sink1;
  out_src = src;
  return true;
}
}  // namespace

auto stage::build(std::string& err) -> bool
{
  m_description = build_description(m_codec, !m_cfg.slate_video_es.empty());

  GError* gerr = nullptr;
  m_pipeline = gst_parse_launch(m_description.c_str(), &gerr);
  if (gerr != nullptr) {
    note(std::format("pipeline warning: {}", gerr->message));
    g_error_free(gerr);
  }
  if (m_pipeline == nullptr) {
    err = "gst_parse_launch returned no pipeline";
    return false;
  }

  const char* const names[] = {"dsrc",  "d",     "vq",    "aq",   "vcap0",
                               "acap0", "vsink", "asink"};
  GstElement** slots[] = {&m_dsrc,    &m_demux,  &m_vqueue, &m_aqueue,
                          &m_vcap0,  &m_acap0, &m_vsink,  &m_asink};
  for (std::size_t i = 0; i < std::size(names); ++i) {
    *slots[i] = gst_bin_get_by_name(GST_BIN(m_pipeline), names[i]);
    if (*slots[i] == nullptr) {
      err = std::string {"pipeline is missing element '"} + names[i] + "'";
      return false;
    }
  }
  if (m_cfg.slate_video_es.empty()) {
    // No slate: the branches are the rings' feed and nothing else. Straight
    // capsfilter -> sink, no selector, no switching to get wrong.
    if (!gst_element_link(m_vcap0, m_vsink)
        || !gst_element_link(m_acap0, m_asink)) {
      err = "could not link the live branches to their sinks";
      return false;
    }
  } else {
    m_vslate_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "svsrc");
    m_aslate_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "sasrc");
    m_vcap1 = gst_bin_get_by_name(GST_BIN(m_pipeline), "vcap1");
    m_acap1 = gst_bin_get_by_name(GST_BIN(m_pipeline), "acap1");
    if (m_vslate_appsrc == nullptr || m_aslate_appsrc == nullptr
        || m_vcap1 == nullptr || m_acap1 == nullptr) {
      err = "pipeline is missing a slate element";
      return false;
    }
    if (!make_selector("vsw", m_vsw.bin, m_vsw.sel, m_vsw.sink0, m_vsw.sink1,
                       m_vsw.src)) {
      err = "could not create the video input-selector bin";
      return false;
    }
    // Hand ownership to the pipeline immediately: destroy() must never have to
    // decide whether a bin was sunk or is still floating.
    gst_bin_add(GST_BIN(m_pipeline), m_vsw.bin);
    if (!make_selector("asw", m_asw.bin, m_asw.sel, m_asw.sink0, m_asw.sink1,
                       m_asw.src)) {
      err = "could not create the audio input-selector bin";
      return false;
    }
    gst_bin_add(GST_BIN(m_pipeline), m_asw.bin);
    if (!gst_element_link_pads(m_vcap0, "src", m_vsw.bin, "sink_0")
        || !gst_element_link_pads(m_vcap1, "src", m_vsw.bin, "sink_1")
        || !gst_element_link_pads(m_vsw.bin, "src", m_vsink, nullptr)
        || !gst_element_link_pads(m_acap0, "src", m_asw.bin, "sink_0")
        || !gst_element_link_pads(m_acap1, "src", m_asw.bin, "sink_1")
        || !gst_element_link_pads(m_asw.bin, "src", m_asink, nullptr)) {
      err = "could not link a selector into its branches";
      return false;
    }
  }

  // Dynamic demux pads: route by caps, never by arrival order.
  g_signal_connect(m_demux, "pad-added", G_CALLBACK(&stage::on_pad_added), this);
  return true;
}

auto stage::destroy() -> void
{
  // Nothing may be pushing into or pulling from the pipeline any more; every
  // caller stops the feed thread and the loopers first.
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);  // joins stream threads
  }
  m_vswitch.detach();
  m_aswitch.detach();

  if (m_vprobe != 0 && m_vprobe_pad != nullptr) {
    gst_pad_remove_probe(m_vprobe_pad, m_vprobe);
  }
  if (m_aprobe != 0 && m_aprobe_pad != nullptr) {
    gst_pad_remove_probe(m_aprobe_pad, m_aprobe);
  }
  m_vprobe = 0;
  m_aprobe = 0;
  if (m_vprobe_pad != nullptr) {
    gst_object_unref(m_vprobe_pad);
    m_vprobe_pad = nullptr;
  }
  if (m_aprobe_pad != nullptr) {
    gst_object_unref(m_aprobe_pad);
    m_aprobe_pad = nullptr;
  }

  for (selector* s : {&m_vsw, &m_asw}) {
    if (s->sink0 != nullptr) {
      gst_object_unref(s->sink0);
    }
    if (s->sink1 != nullptr) {
      gst_object_unref(s->sink1);
    }
    if (s->src != nullptr) {
      gst_object_unref(s->src);
    }
    if (s->sel != nullptr) {
      gst_object_unref(s->sel);
    }
    *s = selector {};
  }

  for (GstElement** e :
       {&m_dsrc, &m_demux, &m_vqueue, &m_aqueue, &m_vcap0, &m_acap0, &m_vcap1,
        &m_acap1, &m_vslate_appsrc, &m_aslate_appsrc, &m_vsink, &m_asink}) {
    if (*e != nullptr) {
      gst_object_unref(*e);
      *e = nullptr;
    }
  }
  if (m_pipeline != nullptr) {
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
  }
}

// ---------------------------------------------------------------------------
// demux pad routing
// ---------------------------------------------------------------------------

auto stage::on_pad_added(GstElement* /*demux*/, GstPad* pad, gpointer user)
    -> void
{
  static_cast<stage*>(user)->link_demux_pad(pad);
}

auto stage::link_demux_pad(GstPad* pad) -> void
{
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps == nullptr) {
    caps = gst_pad_query_caps(pad, nullptr);
  }
  if (caps == nullptr) {
    return;
  }
  GstElement* queue = nullptr;
  if (gst_caps_get_size(caps) > 0) {
    const GstStructure* st = gst_caps_get_structure(caps, 0);
    const char* name = gst_structure_get_name(st);
    if (g_str_has_prefix(name, "video/")) {
      queue = m_vqueue;
    } else if (g_str_has_prefix(name, "audio/")) {
      queue = m_aqueue;
    }
  }
  gst_caps_unref(caps);
  if (queue == nullptr) {
    return;  // subtitles, data, anything else: not this stage's media
  }
  GstPad* sink = gst_element_get_static_pad(queue, "sink");
  if (sink == nullptr) {
    return;
  }
  if (!gst_pad_is_linked(sink) && gst_pad_link(pad, sink) == GST_PAD_LINK_OK) {
    note("stage: routed a demux pad to its branch");
  }
  gst_object_unref(sink);
}

// ---------------------------------------------------------------------------
// switches and probes
// ---------------------------------------------------------------------------

auto stage::video_branches_agree() -> bool
{
  // Before negotiation the pads have no caps; the declared caps are identical by
  // construction, so there is nothing to disagree about yet. Once the branches
  // have negotiated, the REAL caps decide: a width/profile difference is a
  // renegotiation at the selector, which the muxer turns into a split video.
  if (m_vsw.sink0 == nullptr || m_vsw.sink1 == nullptr) {
    return true;
  }
  GstCaps* live = gst_pad_get_current_caps(m_vsw.sink0);
  GstCaps* slate = gst_pad_get_current_caps(m_vsw.sink1);
  bool agree = true;
  if (live != nullptr && slate != nullptr) {
    agree = gst_caps_is_equal(live, slate);
    if (!agree) {
      gchar* a = gst_caps_to_string(live);
      gchar* b = gst_caps_to_string(slate);
      note(std::format("stage: REFUSING the slate -- branch caps differ\n"
                       "  live:  {}\n  slate: {}",
                       a != nullptr ? a : "?", b != nullptr ? b : "?"));
      g_free(a);
      g_free(b);
    }
  }
  if (live != nullptr) {
    gst_caps_unref(live);
  }
  if (slate != nullptr) {
    gst_caps_unref(slate);
  }
  return agree;
}

auto stage::attach_switches() -> bool
{
  if (m_cfg.slate_video_es.empty()) {
    return true;  // no selectors were built
  }
  // One gate per session: the audio switch learns from it what the video
  // selector has actually done, so the two can never cross out of order.
  m_gate = std::make_shared<switch_gate>();

  switch_config video;
  video.audio = false;
  // A copy destination joins the live stream at whatever frame it gets: a
  // mid-GOP join is a frozen picture until the next IDR. A session where every
  // output transcodes does not care, so it flips at once.
  video.wait_for_keyframe = m_cfg.any_copy_output;
  video.gate = m_gate;
  video.live_caps = m_video_caps;
  video.slate_caps = m_video_caps;

  switch_config audio;
  audio.audio = true;
  audio.wait_for_keyframe = false;
  audio.gate = m_gate;
  audio.live_caps = m_audio_caps;
  audio.slate_caps = m_audio_caps;

  if (!m_vswitch.attach(m_vsw.bin, video)) {
    note("stage: the video switch could not be attached");
    return false;
  }
  if (!m_aswitch.attach(m_asw.bin, audio)) {
    note("stage: the audio switch could not be attached");
    return false;
  }
  const bool agree = video_branches_agree();
  m_caps_ok.store(m_vswitch.caps_ok() && m_aswitch.caps_ok() && agree,
                  std::memory_order_release);
  if (!m_caps_ok.load(std::memory_order_acquire)) {
    // Never switch into a caps change: the video stays on live and set_slate()
    // will refuse the slate for the rest of the session.
    m_vswitch.set_slate(false);
    m_aswitch.set_slate(false);
  }
  return true;
}

auto stage::add_probes() -> bool
{
  // With a slate the publish point is the selector's src (the chosen branch);
  // without one it is the live capsfilter, because there is no selector -- the
  // stage is then a plain demux -> ring publisher and still re-stamps.
  m_vprobe_pad = m_vsw.src != nullptr
                     ? GST_PAD(gst_object_ref(m_vsw.src))
                     : gst_element_get_static_pad(m_vcap0, "src");
  m_aprobe_pad = m_asw.src != nullptr
                     ? GST_PAD(gst_object_ref(m_asw.src))
                     : gst_element_get_static_pad(m_acap0, "src");
  if (m_vprobe_pad == nullptr || m_aprobe_pad == nullptr) {
    return false;
  }
  m_vprobe = gst_pad_add_probe(m_vprobe_pad, GST_PAD_PROBE_TYPE_BUFFER,
                               &stage::video_probe_cb, this, nullptr);
  m_aprobe = gst_pad_add_probe(m_aprobe_pad, GST_PAD_PROBE_TYPE_BUFFER,
                               &stage::audio_probe_cb, this, nullptr);
  return m_vprobe != 0 && m_aprobe != 0;
}

auto stage::video_probe_cb(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user)
    -> GstPadProbeReturn
{
  return static_cast<stage*>(user)->publish(info, true);
}

auto stage::audio_probe_cb(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user)
    -> GstPadProbeReturn
{
  return static_cast<stage*>(user)->publish(info, false);
}

auto stage::publish(GstPadProbeInfo* info, bool video) -> GstPadProbeReturn
{
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (buf == nullptr) {
    return GST_PAD_PROBE_OK;
  }
  m_aus_in.fetch_add(1, std::memory_order_relaxed);

  // The probe owns the timestamps from here: the buffer is rewritten in place.
  buf = gst_buffer_make_writable(buf);
  GST_PAD_PROBE_INFO_DATA(info) = buf;

  std::int64_t pts = GST_BUFFER_PTS(buf);
  std::int64_t dts = GST_BUFFER_DTS(buf);
  if (!GST_CLOCK_TIME_IS_VALID(pts)) {
    return GST_PAD_PROBE_DROP;  // a stamp that is not there cannot be rewritten
  }
  if (!GST_CLOCK_TIME_IS_VALID(dts)) {
    dts = pts;
  }
  {
    std::lock_guard<std::mutex> guard(m_timeline_mutex);
    if (m_timeline == nullptr) {
      return GST_PAD_PROBE_DROP;
    }
    const bool accepted = video ? m_timeline->apply_driver(pts, dts)
                                : m_timeline->apply_follower(pts, dts);
    if (!accepted) {
      return GST_PAD_PROBE_DROP;  // malformed stamp; state unchanged
    }
  }

  es_ring* ring = video ? m_cfg.video_ring : m_cfg.audio_ring;
  if (ring != nullptr) {
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
      es_ring::au_meta meta;
      meta.pts_ns = pts;
      meta.dts_ns = dts;
      // Audio has no keyframes. Video's AU is parser-aligned (alignment=au), so
      // the first NAL/OBU is the start of an access unit and the predicate is
      // answering the question it was written for.
      meta.keyframe =
          video ? au_starts_keyframe(m_codec, m_framing, map.data, map.size)
                : true;
      if (ring->write(meta, map.data, map.size)) {
        m_aus_out.fetch_add(1, std::memory_order_relaxed);
      }
      gst_buffer_unmap(buf, &map);
    }
  }

  GST_BUFFER_PTS(buf) = pts;
  GST_BUFFER_DTS(buf) = dts;
  return GST_PAD_PROBE_OK;
}

// ---------------------------------------------------------------------------
// threads
// ---------------------------------------------------------------------------

auto stage::feed_loop() -> void
{
  std::vector<std::uint8_t> chunk(k_read_chunk);
  constexpr auto k_read_timeout = std::chrono::milliseconds {50};

  while (!m_feed_stopping.load(std::memory_order_acquire)) {
    // A branch that has gone silent produces no buffer probes, so a switch
    // waiting for it is also given a deadline check here. Both calls return
    // immediately when nothing is pending.
    m_vswitch.tick();
    m_aswitch.tick();
    if (m_dsrc == nullptr || m_cfg.input == nullptr || m_consumer < 0) {
      std::this_thread::sleep_for(k_idle_sleep);
      continue;
    }
    // Backpressure: the appsrc is block=false, so a full queue means the demux
    // chain has stopped consuming. Hold the ring cursor instead of draining --
    // draining would hand tsdemux a stream with holes in it.
    if (gst_app_src_get_current_level_bytes(GST_APP_SRC(m_dsrc))
        >= k_appsrc_max_bytes) {
      std::this_thread::sleep_for(k_idle_sleep);
      continue;
    }
    for (int i = 0; i < k_feed_burst; ++i) {
      if (m_feed_stopping.load(std::memory_order_acquire)) {
        return;
      }
      const std::size_t got = m_cfg.input->read(m_consumer, chunk.data(),
                                                chunk.size(), k_read_timeout);
      if (got == 0) {
        break;  // timeout or the ring was closed: re-check the world
      }
      GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, got, nullptr);
      gst_buffer_fill(gbuf, 0, chunk.data(), got);
      // Takes ownership of gbuf whether or not the push succeeds.
      if (gst_app_src_push_buffer(GST_APP_SRC(m_dsrc), gbuf) != GST_FLOW_OK) {
        break;
      }
    }
  }
}

auto stage::start_feed() -> void
{
  m_feed_stopping.store(false, std::memory_order_release);
  m_feed = std::thread(&stage::feed_loop, this);
}

auto stage::stop_feed() -> void
{
  m_feed_stopping.store(true, std::memory_order_release);
  if (m_feed.joinable()) {
    m_feed.join();  // bounded by the ring read timeout
  }
}

auto stage::start_loopers() -> bool
{
  m_slate_ready.store(false, std::memory_order_release);
  if (m_cfg.slate_video_es.empty()) {
    return true;
  }
  if (m_timeline == nullptr) {
    return false;
  }
  const std::int64_t frame = m_timeline->frame_duration_ns();
  m_vlooper = std::make_unique<es_looper>(m_vslate_appsrc, m_cfg.slate_video_es,
                                          frame, m_log);
  // es_looper replays an elementary video stream; the audio file is ADTS AAC and
  // is looped by adts_looper (same never-rewinding PTS, different framing).
  m_alooper = std::make_unique<adts_looper>(m_aslate_appsrc,
                                            m_cfg.slate_audio_es, m_log);
  const bool video_ok = m_vlooper->start();
  const bool audio_ok = m_alooper->start();
  if (!video_ok || !audio_ok) {
    // A slate with no video (or no audio) is not a slate: the muxer would drop a
    // pad or stall. Degrade to live rather than publish half a picture.
    note("stage: the slate could not be looped; staying on live");
    stop_loopers();
    return false;
  }
  m_slate_ready.store(true, std::memory_order_release);
  return true;
}

auto stage::stop_loopers() -> void
{
  m_vlooper.reset();
  m_alooper.reset();
}

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------

auto stage::start(const stage_config& cfg) -> bool
{
  std::lock_guard<std::mutex> guard(m_state_mutex);
  if (m_running.load(std::memory_order_acquire)) {
    note("stage: already started");
    return false;
  }
  if (cfg.input == nullptr || cfg.video_ring == nullptr) {
    note("stage: start needs the input TS ring and the video ES ring");
    return false;
  }
  if (cfg.fps <= 0 || cfg.width <= 0 || cfg.height <= 0) {
    note("stage: start needs a positive geometry and frame rate");
    return false;
  }
  if (!parse_codec(cfg.in_codec, m_codec)) {
    note(std::format("stage: unknown in_codec '{}'", cfg.in_codec));
    return false;
  }
  if (cfg.slate_video_es.empty() != cfg.slate_audio_es.empty()) {
    note("stage: a slate needs BOTH an ES video and an ES audio file; "
         "running without a slate");
  }

  m_cfg = cfg;
  if (cfg.slate_video_es.empty() || cfg.slate_audio_es.empty()) {
    m_cfg.slate_video_es.clear();
    m_cfg.slate_audio_es.clear();
  }
  // Every parser in this stack emits Annex-B for h264/h265 and OBU framing for
  // AV1; au_starts_keyframe() reads the right one from the codec and ignores the
  // framing for AV1.
  m_framing = es_nal_framing::annexb;
  m_video_caps = video_caps(m_codec);
  m_audio_caps = "audio/mpeg,mpegversion=4,stream-format=adts,parsed=true";

  // Created once per session: rebuild() must not reset it, or the first AU of
  // the rebuilt pipeline would jump the published timeline back to zero.
  m_timeline = std::make_unique<timeline_state>(
      1'000'000'000LL / cfg.fps, cfg.max_gap_ms * 1'000'000LL);

  std::string err;
  if (!build(err)) {
    note("stage: " + err);
    destroy();
    return false;
  }
  if (!attach_switches() || !add_probes()) {
    destroy();
    return false;
  }

  m_consumer = m_cfg.input->add_consumer();
  if (m_consumer < 0) {
    note("stage: the input ring has no consumer slot");
    destroy();
    return false;
  }

  // Go to PLAYING before anything pushes: the loopers are is-live and the app
  // srcs would otherwise queue into a pipeline that is not running yet.
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    note("stage: the pipeline refused to go to PLAYING");
    destroy();
    m_cfg.input->remove_consumer(m_consumer);
    m_consumer = -1;
    return false;
  }

  start_loopers();
  start_feed();
  m_running.store(true, std::memory_order_release);
  m_desired_slate.store(false, std::memory_order_relaxed);
  set_slate(m_slate_ready.load(std::memory_order_acquire));  // a slate starts on it
  note(std::format("stage: started ({}x{} @ {} fps, slate {})", cfg.width,
                   cfg.height, cfg.fps,
                   m_slate_ready.load(std::memory_order_acquire) ? "ready"
                                                                 : "absent"));
  return true;
}

auto stage::stop() -> void
{
  std::lock_guard<std::mutex> guard(m_state_mutex);
  if (!m_running.load(std::memory_order_acquire) && m_pipeline == nullptr
      && m_consumer < 0) {
    return;  // already stopped (and no half-started session to unwind)
  }
  m_running.store(false, std::memory_order_release);
  stop_feed();
  stop_loopers();
  destroy();
  if (m_consumer >= 0 && m_cfg.input != nullptr) {
    m_cfg.input->remove_consumer(m_consumer);
  }
  m_consumer = -1;
}

auto stage::set_slate(bool slate) -> void
{
  if (!m_running.load(std::memory_order_acquire)) {
    return;  // nothing is running; the request would have no consumer
  }
  if (slate && !m_slate_ready.load(std::memory_order_acquire)) {
    note("stage: slate requested but none is available; staying live");
    return;
  }
  if (slate && !m_caps_ok.load(std::memory_order_acquire)) {
    note("stage: refusing the slate -- the branches' caps differ");
    return;
  }
  const bool changed =
      m_desired_slate.exchange(slate, std::memory_order_acq_rel) != slate;
  // The switches record the request even when they are momentarily detached
  // (rebuild) and apply it on attach, so this is safe at any time.
  if (m_vswitch.attached()) {
    m_vswitch.set_slate(slate);
  }
  if (m_aswitch.attached()) {
    m_aswitch.set_slate(slate);
  }
  if (changed) {
    m_switches.fetch_add(1, std::memory_order_relaxed);
    note(slate ? "stage: slate" : "stage: live");
  }
}

auto stage::rebuild() -> bool
{
  std::lock_guard<std::mutex> guard(m_state_mutex);
  if (!m_running.load(std::memory_order_acquire)) {
    note("stage: rebuild on a stage that is not running");
    return false;
  }
  note("stage: rebuilding the demux/parse/select chain");

  // The writer side goes quiet first: the feed thread (which holds m_dsrc) and
  // the loopers (which hold the slate appsrcs). The ts_ring consumer, the rings
  // and m_timeline are deliberately kept: the cursor survives, so the rebuilt
  // demuxer resumes where the old one stopped, and the first AU continues the
  // published timeline.
  stop_feed();
  stop_loopers();
  destroy();

  std::string err;
  if (!build(err)) {
    note("stage: rebuild failed: " + err);
    m_running.store(false, std::memory_order_release);
    return false;
  }
  if (!attach_switches() || !add_probes()) {
    note("stage: rebuild could not re-attach the switches");
    destroy();
    m_running.store(false, std::memory_order_release);
    return false;
  }
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    note("stage: rebuilt pipeline refused to go to PLAYING");
    destroy();
    m_running.store(false, std::memory_order_release);
    return false;
  }

  start_loopers();
  if (!m_slate_ready.load(std::memory_order_acquire)) {
    // The rebuilt chain has no slate: leaving the switches asking for a branch
    // that is not there would publish nothing at all.
    m_desired_slate.store(false, std::memory_order_relaxed);
    m_vswitch.set_slate(false);
    m_aswitch.set_slate(false);
  }
  start_feed();
  m_rebuilds.fetch_add(1, std::memory_order_relaxed);
  note(std::format("stage: rebuilt ({} so far, timeline last_out {} ms)",
                   m_rebuilds.load(std::memory_order_relaxed),
                   m_timeline != nullptr
                       ? m_timeline->last_out_ns() / 1'000'000
                       : 0));
  return true;
}

}  // namespace standby

#endif  // OBR_HAVE_GSTREAMER
