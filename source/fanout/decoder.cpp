// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/decoder.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <gst/video/video.h>

namespace
{
constexpr std::size_t k_read_chunk = 32 * 1024;
constexpr std::chrono::milliseconds k_read_timeout {0};   // non-blocking feed
constexpr GstClockTime k_pull_timeout = 5 * GST_MSECOND;  // decoded-frame poll
constexpr guint64 k_appsrc_max_bytes = 4 * 1024 * 1024;
// How long the decode appsrc may sit at its byte limit before the stage reports
// itself stalled. The feed thread never blocks on the push, so this is an
// observation, not a timeout that interrupts anything.
constexpr auto k_stall_timeout = std::chrono::seconds(5);
constexpr auto k_stats_period = std::chrono::seconds(2);
// Decoded frames published per feed iteration; the appsink drops beyond its
// own queue, so draining it fully keeps the frame ring current.
constexpr int k_publish_burst = 8;
// How long an attempt may run without emitting a single decoded frame before it
// is retired and the next decoder in the chain is built. A working decoder
// emits inside the first second; this window only has to be longer than a slow
// preroll, and it is the bound that turns a silently wedged hardware decoder
// (pipeline parked in ASYNC, no bus error) into a fallback instead of a stall.
constexpr auto k_attempt_window = std::chrono::seconds(3);
constexpr auto k_ingest_wait = std::chrono::seconds(10);

auto now_steady() -> std::chrono::steady_clock::time_point
{
  return std::chrono::steady_clock::now();
}

auto steady_ns() -> std::int64_t
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             now_steady().time_since_epoch())
      .count();
}

auto registry_has_element(const char* name) -> bool
{
  GstElementFactory* factory = gst_element_factory_find(name);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

// Route tsdemux's dynamic src pads to the video branch BY CAPS. The decode stage
// only needs video: the audio in the ingest is carried independently (the rtmp
// transcode branch re-demuxes the source TS for its AAC), so an audio pad here is
// sunk to the drop funnel.
static void on_demux_pad_added(GstElement* /*demux*/, GstPad* pad, gpointer data)
{
  auto* targets = static_cast<pad_targets*>(data);
  if (targets == nullptr) {
    return;
  }
  GstElement* vqueue = targets->video;
  GstElement* adrop = targets->other;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (caps == nullptr) {
    caps = gst_pad_query_caps(pad, nullptr);
  }
  if (caps == nullptr) {
    return;
  }
  const char* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  const bool is_video = name != nullptr && g_str_has_prefix(name, "video/");
  gst_caps_unref(caps);

  GstPadLinkReturn lr = GST_PAD_LINK_REFUSED;
  if (is_video && vqueue != nullptr) {
    GstPad* sink = gst_element_get_static_pad(vqueue, "sink");
    if (sink != nullptr) {
      if (!gst_pad_is_linked(sink)) {
        lr = gst_pad_link(pad, sink);
      }
      gst_object_unref(sink);
    }
    if (lr != GST_PAD_LINK_OK) {
      fprintf(stderr, "[decoder] demux video pad link failed: %s\n",
              gst_pad_link_get_name(lr));
    }
    return;
  }

  // Non-video: a fresh request pad on the funnel. Leaving it unlinked is not an
  // option -- tsdemux returns GST_FLOW_NOT_LINKED and stops the whole demux.
  if (adrop == nullptr) {
    return;
  }
  GstPad* fsink = gst_element_request_pad_simple(adrop, "sink_%u");
  if (fsink == nullptr) {
    fprintf(stderr, "[decoder] no drop funnel pad for a non-video pad\n");
    return;
  }
  lr = gst_pad_link(pad, fsink);
  if (lr != GST_PAD_LINK_OK) {
    fprintf(stderr, "[decoder] demux non-video pad link failed: %s\n",
            gst_pad_link_get_name(lr));
  }
  gst_object_unref(fsink);
}

// Counts buffers on one pad. Attached to the pads whose counters are reported in
// the periodic line, so a stall in the decode chain can be located to a stage
// instead of inferred from the pipeline state.
auto count_probe(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer data)
    -> GstPadProbeReturn
{
  auto* counter = static_cast<std::atomic<std::uint64_t>*>(data);
  counter->fetch_add(1, std::memory_order_relaxed);
  return GST_PAD_PROBE_PASS;
}

// The parse string leaves elements unnamed, so gst_parse_launch names each one
// after its factory ("vaav1dec0").
auto find_child(GstElement* pipeline, const char* factory) -> GstElement*
{
  GstElement* direct = gst_bin_get_by_name(GST_BIN(pipeline), factory);
  if (direct != nullptr) {
    return direct;
  }
  GstElement* found = nullptr;
  GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipeline));
  GValue item = G_VALUE_INIT;
  bool done = false;
  while (!done) {
    switch (gst_iterator_next(it, &item)) {
      case GST_ITERATOR_OK: {
        auto* elem = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* fac = gst_element_get_factory(elem);
        if (fac != nullptr
            && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(fac)),
                         factory) == 0) {
          found = GST_ELEMENT(gst_object_ref(elem));
          done = true;
        }
        g_value_reset(&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync(it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = true;
        break;
    }
  }
  gst_iterator_free(it);
  return found;
}
}  // namespace

auto decoder_template(codec /*in_codec*/,
                      const char* source_parser,
                      const char* decoder) -> std::string
{
  // The parse element is chosen from the source codec and the decoder from the
  // same alternatives list required_elements() preflights (lib.h), so the
  // runtime can never pick an element the preflight did not check.
  // Branch queues are named and linked per pad: tsdemux src pads are dynamic,
  // and a pad-agnostic `d.` link would hand the audio pad to the video branch.
  // The demuxer is deliberately NOT linked to the branch in the template: a
  // static `d ! queue` in a parse string installs gst_parse_launch's own
  // delayed link, which attaches whichever pad appears FIRST (the audio pad on
  // this ingest) to the video branch. build_pipeline() routes the pads by caps
  // instead, and the unlinked queue head is tolerated by gst_parse_launch.
  return std::format(
      // tsparse requires explicit caps on its source.
      "appsrc name=dsrc is-live=true do-timestamp=true format=time "
      "block=false max-bytes=4194304 caps=video/mpegts,systemstream=true "
      "! tsparse set-timestamps=true alignment=7 "
      "! tsdemux name=d "
      "queue name=vqueue ! {} ! {} ! videoconvert ! video/x-raw,format=NV12 "
      "! appsink name=dsink sync=false max-buffers=8 drop=true "
      // Everything that is not video is discarded through the funnel. `async=false`
      // is REQUIRED, not cosmetic: a sink that never prerolls (because its pad
      // never appears, or has not yet) keeps the whole pipeline in ASYNC state
      // forever, which is indistinguishable from a hang: no errors, no frames.
      "funnel name=adrop ! fakesink sync=false async=false",
      source_parser,
      decoder);
}

decoder::decoder(ts_ring& ring, codec in_codec, log_fn log)
    : m_ring {ring}
    , m_in_codec {in_codec}
    , m_log {std::move(log)}
{
}

decoder::~decoder()
{
  stop();
}

auto decoder::log(const std::string& msg) const -> void
{
  if (m_log) {
    m_log("[decoder] " + msg);
  }
}

auto decoder::counters() const -> stage_counters
{
  stage_counters out;
  out.in_chunks = m_in_chunks.load(std::memory_order_relaxed);
  out.demux_buffers = m_demux_buffers.load(std::memory_order_relaxed);
  out.decoder_buffers = m_decoder_buffers.load(std::memory_order_relaxed);
  out.sink_buffers = m_sink_buffers.load(std::memory_order_relaxed);
  out.stalled = m_stalled.load(std::memory_order_relaxed);
  out.failed = m_failed.load(std::memory_order_relaxed);
  if (m_pipeline != nullptr) {
    GstState pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(m_pipeline, &out.state, &pending, 0);
  }
  return out;
}

auto decoder::stop() -> void
{
  m_stopping.store(true, std::memory_order_release);
  // Everything that can RELEASE a blocked thread happens before the join, because
  // a thread parked in a GStreamer call or waiting on the frame ring never sees
  // m_stopping: the NULL transition frees the former, m_frames.close() wakes the
  // latter. Joining first would block stop for as long as the park lasts.
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
  }
  m_frames.close();  // wake every blocked transcode consumer
  if (m_thread.joinable()) {
    m_thread.join();
  }
  destroy_pipeline();
  if (m_consumer >= 0) {
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
  }
}

auto decoder::destroy_pipeline() -> void
{
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
  }
  if (m_bus != nullptr) {
    gst_object_unref(m_bus);
    m_bus = nullptr;
  }
  if (m_appsrc != nullptr) {
    gst_object_unref(m_appsrc);
    m_appsrc = nullptr;
  }
  if (m_appsink != nullptr) {
    gst_object_unref(m_appsink);
    m_appsink = nullptr;
  }
  if (m_pipeline != nullptr) {
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
  }
}

auto decoder::build_pipeline(std::string& err, const std::string& decoder_name)
    -> bool
{
  const auto parser = choose_present(
      transcode_source_parser_alternatives(m_in_codec), &registry_has_element);
  if (!parser) {
    err = std::format("no parser for source codec {}",
                      to_string(m_in_codec));
    return false;
  }

  const std::string tmpl =
      decoder_template(m_in_codec, parser->c_str(), decoder_name.c_str());
  // Log the elements actually chosen and the exact pipeline. Every external factor
  // has been checked (bytes, demuxer, plugins, GStreamer version, GPU, permissions)
  // and the same template succeeds outside the receiver, so the remaining question
  // is what THIS process builds and what it selects at runtime.
  fprintf(stderr, "[decoder] source codec=%s parser=%s decoder=%s\n",
          to_string(m_in_codec), parser->c_str(), decoder_name.c_str());
  fprintf(stderr, "[decoder] pipeline: %s\n", tmpl.c_str());
  GError* gerr = nullptr;
  m_pipeline = gst_parse_launch(tmpl.c_str(), &gerr);
  if (m_pipeline == nullptr || gerr != nullptr) {
    err = (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                        : "gst_parse_launch";
    if (gerr != nullptr) {
      g_error_free(gerr);
    }
    destroy_pipeline();
    return false;
  }

  // Link tsdemux's src pads by caps -- never by a bare `d.` request (see the
  // template comment). The queue is created by the parse and left unlinked, which
  // gst_parse_launch tolerates without error (verified on 1.28.6).
  GstElement* demux = gst_bin_get_by_name(GST_BIN(m_pipeline), "d");
  GstElement* vqueue = gst_bin_get_by_name(GST_BIN(m_pipeline), "vqueue");
  GstElement* adrop = gst_bin_get_by_name(GST_BIN(m_pipeline), "adrop");
  if (demux == nullptr || vqueue == nullptr || adrop == nullptr) {
    err = "decode pipeline is missing d/vqueue/adrop";
    if (demux != nullptr) { gst_object_unref(demux); }
    if (vqueue != nullptr) { gst_object_unref(vqueue); }
    if (adrop != nullptr) { gst_object_unref(adrop); }
    destroy_pipeline();
    return false;
  }
  pad_targets targets {vqueue, adrop};
  m_pad_targets = targets;
  g_signal_connect(demux, "pad-added", G_CALLBACK(on_demux_pad_added), &m_pad_targets);
  gst_object_unref(demux);
  gst_object_unref(vqueue);
  gst_object_unref(adrop);

  m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "dsrc");
  m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "dsink");
  if (m_appsrc == nullptr || m_appsink == nullptr) {
    err = "decode pipeline is missing dsrc/dsink";
    destroy_pipeline();
    return false;
  }
  gst_app_src_set_max_bytes(GST_APP_SRC(m_appsrc), k_appsrc_max_bytes);
  attach_probes(decoder_name.c_str());
  m_bus = gst_element_get_bus(m_pipeline);
  return true;
}

auto decoder::attach_probes(const char* decoder_name) -> void
{
  // vqueue's sink pad receives exactly what the demuxer's video pad produced.
  GstElement* vqueue = gst_bin_get_by_name(GST_BIN(m_pipeline), "vqueue");
  if (vqueue != nullptr) {
    GstPad* sink = gst_element_get_static_pad(vqueue, "sink");
    if (sink != nullptr) {
      gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, &count_probe,
                        &m_demux_buffers, nullptr);
      gst_object_unref(sink);
    }
    gst_object_unref(vqueue);
  }
  // Probe the decoder's src pad, not its sink: the sink pad can accept a buffer
  // the decoder then never emits (a frame it cannot complete).
  GstElement* dec = find_child(m_pipeline, decoder_name);
  if (dec != nullptr) {
    GstPad* src = gst_element_get_static_pad(dec, "src");
    if (src != nullptr) {
      gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, &count_probe,
                        &m_decoder_buffers, nullptr);
      gst_object_unref(src);
    }
    gst_object_unref(dec);
  } else {
    log("cannot find " + std::string {decoder_name}
        + " to probe; decoder counts stay 0\n");
  }
  GstPad* sink = gst_element_get_static_pad(m_appsink, "sink");
  if (sink != nullptr) {
    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, &count_probe,
                      &m_sink_buffers, nullptr);
    gst_object_unref(sink);
  }
}

auto decoder::log_stats() -> void
{
  GstState state = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn scr =
      m_pipeline != nullptr ? gst_element_get_state(m_pipeline, &state, &pending, 0)
                            : GST_STATE_CHANGE_FAILURE;
  const guint64 level = m_appsrc != nullptr
      ? gst_app_src_get_current_level_bytes(GST_APP_SRC(m_appsrc))
      : 0;
  log(std::format(
      "in={} chunks/{} KiB demux={} decoded={} sink={} level={} KiB "
      "dec={} fallback={} state={} ({}) pending={} stalled={}\n",
      m_in_chunks.load(std::memory_order_relaxed),
      m_in_bytes.load(std::memory_order_relaxed) / 1024U,
      m_demux_buffers.load(std::memory_order_relaxed),
      m_decoder_buffers.load(std::memory_order_relaxed),
      m_sink_buffers.load(std::memory_order_relaxed),
      level / 1024U,
      decoder_element().empty() ? "none" : decoder_element().c_str(),
      m_using_fallback.load(std::memory_order_relaxed) ? 1 : 0,
      gst_element_state_get_name(state),
      gst_element_state_change_return_get_name(scr),
      gst_element_state_get_name(pending),
      m_stalled.load(std::memory_order_relaxed) ? 1 : 0));
}

auto decoder::start_attempt(std::size_t index, std::string& err) -> bool
{
  m_attempt = index;
  {
    const std::scoped_lock lock {m_name_mutex};
    m_decoder_name = m_chain[index];
  }
  m_using_fallback.store(index > 0, std::memory_order_relaxed);

  if (!build_pipeline(err, m_decoder_name)) {
    destroy_pipeline();
    return false;
  }
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    err = "pipeline refused PLAYING";
    destroy_pipeline();
    return false;
  }

  // Report what the pipeline reached and whether the demux exposed anything: a
  // bare "Internal data stream error" says nothing about which of the two
  // failed, and this is cheap. ASYNC is expected here -- preroll completes in
  // the background and the no-frame window is what bounds it.
  GstState st = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn scr =
      gst_element_get_state(m_pipeline, &st, &pending, GST_SECOND);
  log(std::format("{} attempt decoder={} state={} ({}) pending={}\n",
                  index == 0 ? "primary" : "fallback", m_decoder_name,
                  gst_element_state_get_name(st),
                  gst_element_state_change_return_get_name(scr),
                  gst_element_state_get_name(pending)));

  m_attempt_start = now_steady();
  m_attempt_base_decoder = m_decoder_buffers.load(std::memory_order_relaxed);
  m_stall_since = std::chrono::steady_clock::time_point {};
  return true;
}

auto decoder::degrade(const std::string& reason) -> bool
{
  const std::string failed_name = m_decoder_name;

  // Tear down first: the log line that follows describes an attempt that no
  // longer exists, and the stage counters are reset so the next stats line is
  // about the new attempt rather than the dead one.
  destroy_pipeline();
  m_demux_buffers.store(0, std::memory_order_relaxed);
  m_decoder_buffers.store(0, std::memory_order_relaxed);
  m_sink_buffers.store(0, std::memory_order_relaxed);
  m_stall_since = std::chrono::steady_clock::time_point {};

  if (m_attempt + 1 >= m_chain.size()) {
    log(std::format(
        "decoder {} failed: {}; no decoder left for {} -- transcode outputs "
        "stop here\n",
        failed_name, reason, to_string(m_in_codec)));
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  log(std::format(
      "decoder {} failed: {}; rebuilding with {} (software fallback)\n",
      failed_name, reason, m_chain[m_attempt + 1]));

  while (m_attempt + 1 < m_chain.size()) {
    std::string err;
    if (start_attempt(m_attempt + 1, err)) {
      log(std::format("fallback decoder {} is running ({} failed)\n",
                      m_decoder_name, failed_name));
      return true;
    }
    log(std::format("fallback decoder {} could not be built: {}\n",
                    m_chain[m_attempt + 1], err));
  }

  log(std::format("no decoder left for {} -- transcode outputs stop here\n",
                  to_string(m_in_codec)));
  m_failed.store(true, std::memory_order_relaxed);
  return false;
}

auto decoder::check_attempt_health() -> bool
{
  if (m_simulate_failure && m_attempt == 0) {
    // Test hook: the same retire-and-rebuild path a real failure takes.
    m_simulate_failure = false;
    return degrade("simulated decoder failure (test hook)");
  }

  if (m_bus != nullptr) {
    GstMessage* msg = gst_bus_pop_filtered(m_bus, GST_MESSAGE_ERROR);
    if (msg != nullptr) {
      GError* gerr = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &gerr, &dbg);
      // Name the element and keep GStreamer's own debug string: the generic
      // "Internal data stream error" only says SOMETHING failed to negotiate,
      // `dbg` carries which element and the flow error (not-negotiated (-4)).
      GstObject* src = GST_MESSAGE_SRC(msg);
      const std::string who =
          (src != nullptr && GST_OBJECT_NAME(src) != nullptr)
          ? GST_OBJECT_NAME(src)
          : "?";
      std::string text =
          (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                        : "unknown";
      if (dbg != nullptr) {
        std::string detail {dbg};
        std::replace(detail.begin(), detail.end(), '\n', ' ');
        if (detail.size() > 400) {
          detail.resize(400);
          detail += "...";
        }
        text += " -- " + detail;
      }
      if (gerr != nullptr) {
        g_error_free(gerr);
      }
      g_free(dbg);
      gst_message_unref(msg);  // before degrade: it frees the pipeline
      return degrade(who + " posted " + text);
    }
  }

  // No bus error. A decoder that emitted nothing inside the window is just as
  // dead: av1parse on this box posts NOT_NEGOTIATED while the pipeline is still
  // prerolling, so the error can arrive after -- or not at all -- and this
  // window is what still makes the stage recover.
  if (m_pipeline != nullptr && m_in_chunks.load(std::memory_order_relaxed) > 0
      && m_attempt_start != std::chrono::steady_clock::time_point {}
      && now_steady() - m_attempt_start >= k_attempt_window
      && m_decoder_buffers.load(std::memory_order_relaxed)
             == m_attempt_base_decoder) {
    GstState st = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(m_pipeline, &st, &pending, 0);
    log_stats();
    return degrade(std::format("no decoded frame in {} s (state={}, pending={})",
                               k_attempt_window.count(),
                               gst_element_state_get_name(st),
                               gst_element_state_get_name(pending)));
  }
  return false;
}

auto decoder::start_attempt(std::size_t index, std::string& err) -> bool
{
  m_attempt = index;
  {
    const std::scoped_lock lock {m_name_mutex};
    m_decoder_name = m_chain[index];
  }
  m_using_fallback.store(index > 0, std::memory_order_relaxed);

  if (!build_pipeline(err, m_decoder_name)) {
    destroy_pipeline();
    return false;
  }
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE) {
    err = "pipeline refused PLAYING";
    destroy_pipeline();
    return false;
  }

  // Report what the pipeline reached and whether the demux exposed anything: a
  // bare "Internal data stream error" says nothing about which of the two
  // failed, and this is cheap. ASYNC is expected here -- preroll completes in
  // the background and the no-frame window is what bounds it.
  GstState st = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn scr =
      gst_element_get_state(m_pipeline, &st, &pending, GST_SECOND);
  log(std::format("{} attempt decoder={} state={} ({}) pending={}\n",
                  index == 0 ? "primary" : "fallback", m_decoder_name,
                  gst_element_state_get_name(st),
                  gst_element_state_change_return_get_name(scr),
                  gst_element_state_get_name(pending)));

  m_attempt_start_ns.store(steady_ns(), std::memory_order_relaxed);
  m_attempt_base_decoder.store(
      m_decoder_buffers.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  m_stall_since = std::chrono::steady_clock::time_point {};
  return true;
}

auto decoder::degrade(const std::string& reason) -> bool
{
  const std::string failed_name = m_decoder_name;

  // Tear down first: the log line that follows describes an attempt that no
  // longer exists, and the stage counters are reset so the next stats line is
  // about the new attempt rather than the dead one.
  destroy_pipeline();
  m_demux_buffers.store(0, std::memory_order_relaxed);
  m_decoder_buffers.store(0, std::memory_order_relaxed);
  m_sink_buffers.store(0, std::memory_order_relaxed);
  m_stall_since = std::chrono::steady_clock::time_point {};

  if (m_attempt + 1 >= m_chain.size()) {
    log(std::format(
        "decoder {} failed: {}; no decoder left for {} -- transcode outputs "
        "stop here\n",
        failed_name, reason, to_string(m_in_codec)));
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  log(std::format(
      "decoder {} failed: {}; rebuilding with {} (software fallback)\n",
      failed_name, reason, m_chain[m_attempt + 1]));

  while (m_attempt + 1 < m_chain.size()) {
    std::string err;
    if (start_attempt(m_attempt + 1, err)) {
      log(std::format("fallback decoder {} is running ({} failed)\n",
                      m_decoder_name, failed_name));
      return true;
    }
    log(std::format("fallback decoder {} could not be built: {}\n",
                    m_chain[m_attempt + 1], err));
  }

  log(std::format("no decoder left for {} -- transcode outputs stop here\n",
                  to_string(m_in_codec)));
  m_failed.store(true, std::memory_order_relaxed);
  return false;
}

auto decoder::check_attempt_health() -> bool
{
  if (m_simulate_failure && m_attempt == 0) {
    // Test hook: the same retire-and-rebuild path a real failure takes.
    m_simulate_failure = false;
    return degrade("simulated decoder failure (test hook)");
  }

  if (m_bus != nullptr) {
    GstMessage* msg = gst_bus_pop_filtered(m_bus, GST_MESSAGE_ERROR);
    if (msg != nullptr) {
      GError* gerr = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &gerr, &dbg);
      // Name the element and keep GStreamer's own debug string: the generic
      // "Internal data stream error" only says SOMETHING failed to negotiate,
      // `dbg` carries which element and the flow error (not-negotiated (-4)).
      GstObject* src = GST_MESSAGE_SRC(msg);
      const std::string who =
          (src != nullptr && GST_OBJECT_NAME(src) != nullptr)
          ? GST_OBJECT_NAME(src)
          : "?";
      std::string text =
          (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                        : "unknown";
      if (dbg != nullptr) {
        std::string detail {dbg};
        std::replace(detail.begin(), detail.end(), '\n', ' ');
        if (detail.size() > 400) {
          detail.resize(400);
          detail += "...";
        }
        text += " -- " + detail;
      }
      if (gerr != nullptr) {
        g_error_free(gerr);
      }
      g_free(dbg);
      gst_message_unref(msg);  // before degrade: it frees the pipeline
      return degrade(who + " posted " + text);
    }
  }

  // No bus error. A decoder that emitted nothing inside the window is just as
  // dead: av1parse on this box posts NOT_NEGOTIATED while the pipeline is still
  // prerolling, so the error can arrive after -- or not at all -- and this
  // window is what still makes the stage recover.
  const auto attempt_start_ns = m_attempt_start_ns.load(std::memory_order_relaxed);
  const bool fed = m_in_chunks.load(std::memory_order_relaxed) > 0;
  const auto window = (fed || m_ever_fed.load(std::memory_order_relaxed))
                          ? k_attempt_window
                          : k_ingest_wait;
  if (m_pipeline != nullptr && attempt_start_ns != 0
      && steady_ns() - attempt_start_ns
             >= std::chrono::duration_cast<std::chrono::nanoseconds>(window)
                    .count()) {
    if (!fed) {
      GstState st = GST_STATE_VOID_PENDING;
      GstState pending = GST_STATE_VOID_PENDING;
      gst_element_get_state(m_pipeline, &st, &pending, 0);
      log_stats();
      return degrade(std::format("no TS chunk in {} s (state={}, pending={})",
                                 k_ingest_wait.count(),
                                 gst_element_state_get_name(st),
                                 gst_element_state_get_name(pending)));
    }
    if (m_decoder_buffers.load(std::memory_order_relaxed)
        == m_attempt_base_decoder.load(std::memory_order_relaxed)) {
      GstState st = GST_STATE_VOID_PENDING;
      GstState pending = GST_STATE_VOID_PENDING;
      gst_element_get_state(m_pipeline, &st, &pending, 0);
      log_stats();
      return degrade(std::format("no decoded frame in {} s (state={}, pending={})",
                                 k_attempt_window.count(),
                                 gst_element_state_get_name(st),
                                 gst_element_state_get_name(pending)));
    }
  }
  return false;
}

auto decoder::start() -> bool
{
  m_consumer = m_ring.add_consumer();
  if (m_consumer < 0) {
    log("no ts_ring consumer slot\n");
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  m_chain = transcode_decoder_chain(m_in_codec, &registry_has_element);
  if (m_chain.empty()) {
    log(std::format("no decoder for source codec {}\n", to_string(m_in_codec)));
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  m_in_chunks.store(0, std::memory_order_relaxed);
  m_in_bytes.store(0, std::memory_order_relaxed);
  m_demux_buffers.store(0, std::memory_order_relaxed);
  m_decoder_buffers.store(0, std::memory_order_relaxed);
  m_sink_buffers.store(0, std::memory_order_relaxed);
  m_stall_since = std::chrono::steady_clock::time_point {};

  // The hardware decoder is the preference; it is also disposable. Building all
  // of them up front would mean nothing here -- one tsdemux cannot feed two
  // decoders -- so only the first attempt is built, and the rest are built on
  // demand by degrade() from the feeder thread.
  bool launched = false;
  for (std::size_t idx = 0; idx < m_chain.size() && !launched; ++idx) {
    std::string err;
    launched = start_attempt(idx, err);
    if (!launched) {
      log(std::format("decoder {} could not be built: {}\n", m_chain[idx], err));
    }
  }
  if (!launched) {
    log(std::format("no usable decoder for source codec {}\n",
                    to_string(m_in_codec)));
    destroy_pipeline();
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  m_stopping.store(false, std::memory_order_release);
  m_thread = std::thread([this]() -> void { worker(); });
  log("decode stage running\n");
  return true;
}

auto decoder::publish_sample(GstSample* sample) -> void
{
  GstBuffer* buf = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);
  if (buf == nullptr || caps == nullptr) {
    return;
  }

  GstVideoInfo info;
  gst_video_info_init(&info);
  if (!gst_video_info_from_caps(&info, caps)) {
    return;
  }

  frame_ring::frame_meta meta;
  meta.pts = GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf) : 0;
  meta.dts = GST_BUFFER_DTS_IS_VALID(buf) ? GST_BUFFER_DTS(buf) : 0;
  // An absent DELTA_UNIT flag means a keyframe (IDR / sync point).
  meta.flags = GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)
      ? 0U
      : frame_ring::k_flag_keyframe;
  meta.format = static_cast<std::int32_t>(GST_VIDEO_INFO_FORMAT(&info));
  meta.stride = static_cast<std::int32_t>(GST_VIDEO_INFO_PLANE_STRIDE(&info, 0));

  GstMapInfo map;
  if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
    return;
  }
  // Publish whole or not at all (frame_ring::write drops an empty/oversized
  // frame); the producer never blocks.
  m_frames.write(meta, map.data, map.size);
  gst_buffer_unmap(buf, &map);
}

auto decoder::worker() -> void
{
  std::vector<std::uint8_t> tsbuf(k_read_chunk);
  bool announced_first_feed = false;
  bool stall_reported = false;
  auto last_stats = now_steady();

  while (!m_stopping.load(std::memory_order_acquire)) {
    // 1) health: a posted error, or an attempt that produced no frame inside the
    //    window, is retired here and the next decoder is built. The pipeline
    //    this loop feeds is replaced underneath it; everything below reads the
    //    current attempt's elements, so the loop never holds a stale pointer.
    if (check_attempt_health()) {
      last_stats = now_steady();
      continue;
    }

    // 2) feed: ts_ring -> dsrc. The appsrc is block=false, so a full queue means
    //    the decode chain has stopped consuming: hold the ring cursor instead of
    //    pushing (draining on would hand tsdemux a stream with holes in it) and
    //    keep this loop alive so the bus, the pull and the report still run.
    if (m_appsrc == nullptr) {
      // Every attempt failed: keep the stage alive so stop() is prompt and the
      // periodic line keeps reporting that nothing is decoding.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } else if (gst_app_src_get_current_level_bytes(GST_APP_SRC(m_appsrc))
               >= k_appsrc_max_bytes) {
      const auto now = now_steady();
      if (m_stall_since == std::chrono::steady_clock::time_point {}) {
        m_stall_since = now;
        m_stalled.store(true, std::memory_order_relaxed);
        log("decode appsrc full: the decode chain is not consuming\n");
      } else if (!stall_reported && now - m_stall_since > k_stall_timeout) {
        stall_reported = true;
        log("decode stage stalled: appsrc full for over "
            + std::to_string(k_stall_timeout.count()) + " s\n");
        log_stats();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } else {
      if (m_stall_since != std::chrono::steady_clock::time_point {}) {
        m_stall_since = std::chrono::steady_clock::time_point {};
        m_stalled.store(false, std::memory_order_relaxed);
        stall_reported = false;
        log("decode appsrc draining again\n");
      }
      for (;;) {
        const std::size_t got = m_ring.read(
            m_consumer, tsbuf.data(), tsbuf.size(), k_read_timeout);
        if (got == 0) {
          break;
        }
        if (!announced_first_feed) {
          log("first TS chunk fed to the decode stage: " + std::to_string(got)
              + " bytes\n");
          announced_first_feed = true;
          m_ever_fed.store(true, std::memory_order_relaxed);
          m_attempt_start_ns.store(steady_ns(), std::memory_order_relaxed);
          m_attempt_base_decoder.store(
              m_decoder_buffers.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
        }
        GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, got, nullptr);
        gst_buffer_fill(gbuf, 0, tsbuf.data(), got);
        if (gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), gbuf)
            != GST_FLOW_OK) {
          break;
        }
        m_in_chunks.fetch_add(1, std::memory_order_relaxed);
        m_in_bytes.fetch_add(got, std::memory_order_relaxed);
      }
    }

    // 3) publish decoded frames (bounded burst keeps the loop responsive).
    if (m_appsink != nullptr) {
      for (int pulled = 0; pulled < k_publish_burst; ++pulled) {
        GstSample* sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(m_appsink), pulled == 0 ? k_pull_timeout : 0);
        if (sample == nullptr) {
          break;
        }
        publish_sample(sample);
        gst_sample_unref(sample);
      }
    }

    const auto now = now_steady();
    if (now - last_stats >= k_stats_period) {
      last_stats = now;
      log_stats();
    }
  }
}
