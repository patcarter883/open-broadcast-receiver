// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/decoder.h"

#include <chrono>
#include <format>
#include <string>
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
// ignored rather than linked to something that cannot take it.
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
  // stderr, not the receiver's log: this runs from a GStreamer streaming thread
  // and must not touch anything that could deadlock, and it still lands in
  // `docker logs`. Whether this fires AT ALL is the decisive fact.
  fprintf(stderr, "[decoder] demux pad-added: caps=%s video=%d\n",
          name != nullptr ? name : "(null)", is_video ? 1 : 0);
  gst_caps_unref(caps);
  // Video goes to the branch; every other pad goes to adrop. Leaving a pad
  // unlinked is NOT safe: tsdemux returns GST_FLOW_NOT_LINKED until it has a
  // linked src pad, and the live ingest exposes audio first.
  GstElement* dest = is_video ? vqueue : adrop;
  if (dest == nullptr) {
    return;
  }
  GstPad* sink = gst_element_get_static_pad(dest, "sink");
  if (sink != nullptr) {
    if (!gst_pad_is_linked(sink)) {
      const GstPadLinkReturn lr = gst_pad_link(pad, sink);
      fprintf(stderr, "[decoder] demux pad link (%s) -> %s (%d)\n",
              is_video ? "video" : "other", gst_pad_link_get_name(lr), (int)lr);
    }
    gst_object_unref(sink);
  }
}
}  // namespace

auto decoder_template(codec /*in_codec*/,
                      const char* source_parser,
                      const char* decoder) -> std::string
{
  // The parse element is chosen from the source codec and the decoder from the
  // same alternatives list required_elements() preflights (lib.h), so the
  // runtime can never pick an element the preflight did not check.
  // The video branch queue is named and referenced with NO `d.` request: tsdemux
  // src pads are dynamic, and on GStreamer 1.28.6 a pad-agnostic `d.` link can
  // hand the AUDIO pad to the video branch. With an AV1+AAC ingest that fed AAC
  // into av1parse and killed the whole decode stage with "Internal data stream
  // error", starving every transcode output while the copy path stayed healthy
  // (DT-12 -- the encoder hit exactly this; the decode stage had not).
  return std::format(
      // caps=video/mpegts,systemstream=true is MANDATORY: tsparse will not link
      // to an uncaps'd source, and the failure surfaces as a bare
      // "Internal data stream error" on the decode stage -- starving every
      // transcode output while the copy path (which never parses) stays healthy.
      "appsrc name=dsrc is-live=true do-timestamp=true format=time "
      "block=true max-bytes=4194304 caps=video/mpegts,systemstream=true "
      "! tsparse set-timestamps=true alignment=7 "
      "! tsdemux name=d "
      "queue name=vqueue ! {} ! {} ! videoconvert ! video/x-raw,format=NV12 "
      "! appsink name=dsink sync=false max-buffers=8 drop=true "
      // adrop exists so EVERY demux src pad can be linked. tsdemux emits its pads
      // as it parses, and it returns GST_FLOW_NOT_LINKED while it has NO linked src
      // pad -- so if only the video pad is consumed and the audio pad is exposed
      // first (which the live ingest does), the very first push into the decode
      // appsrc is fatal and the video pad is never reached. Sinking the non-video
      // pads costs nothing and removes the race entirely.
      "fakesink name=adrop sync=false",
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

auto decoder::build_pipeline(std::string& err) -> bool
{
  const auto parser = choose_present(
      transcode_source_parser_alternatives(m_in_codec), &registry_has_element);
  const auto decode = choose_present(
      transcode_decoder_alternatives(m_in_codec), &registry_has_element);
  if (!parser || !decode) {
    err = std::format("no decoder for source codec {}",
                      to_string(m_in_codec));
    return false;
  }

  const std::string tmpl =
      decoder_template(m_in_codec, parser->c_str(), decode->c_str());
  // Log the elements actually chosen and the exact pipeline. Every external factor
  // has been checked (bytes, demuxer, plugins, GStreamer version, GPU, permissions)
  // and the same template succeeds outside the receiver, so the remaining question
  // is what THIS process builds and what it selects at runtime.
  fprintf(stderr, "[decoder] source codec=%s parser=%s decoder=%s\n",
          to_string(m_in_codec), parser->c_str(), decode->c_str());
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
  m_bus = gst_element_get_bus(m_pipeline);
  return true;
}

auto decoder::start() -> bool
{
  m_consumer = m_ring.add_consumer();
  if (m_consumer < 0) {
    log("no ts_ring consumer slot\n");
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  std::string err;
  if (!build_pipeline(err)) {
    log("pipeline build failed: " + err + "\n");
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE)
  {
    log("pipeline refused PLAYING\n");
    destroy_pipeline();
    m_ring.remove_consumer(m_consumer);
    m_consumer = -1;
    m_failed.store(true, std::memory_order_relaxed);
    return false;
  }

  // Report what the pipeline actually reached and whether the demux exposed
  // anything: a bare "Internal data stream error" says nothing about which of the
  // two failed, and this is cheap.
  GstState st = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn scr =
      gst_element_get_state(m_pipeline, &st, &pending, GST_SECOND);
  GstElement* d = gst_bin_get_by_name(GST_BIN(m_pipeline), "d");
  int src_pads = 0;
  if (d != nullptr) {
    GstIterator* it = gst_element_iterate_src_pads(d);
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
      src_pads++;
      g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    gst_object_unref(d);
  }
  log(std::format("pipeline state={} ({}) demux src pads={}\n",
                  static_cast<int>(st),
                  gst_element_state_change_return_get_name(scr), src_pads));

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
  bool announced_failure = false;
  bool announced_first_feed = false;
  std::size_t total_fed = 0;

  while (!m_stopping.load(std::memory_order_acquire)) {
    // 1) bus: surface a decoder error once, then keep draining the ring so a
    //    transient error does not wedge the stage; copy outputs are unaffected.
    while (m_bus != nullptr) {
      GstMessage* msg =
          gst_bus_pop_filtered(m_bus, GST_MESSAGE_ERROR);
      if (msg == nullptr) {
        break;
      }
      GError* gerr = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &gerr, &dbg);
      if (!announced_failure) {
        // Name the element and keep GStreamer's own debug string. The generic
        // "Internal data stream error" only says SOMETHING failed to negotiate;
        // `dbg` carries which element and why, and it was being read and thrown
        // away -- which is why this stage could fail with no usable evidence.
        GstObject* src = GST_MESSAGE_SRC(msg);
        const std::string who =
            (src != nullptr && GST_OBJECT_NAME(src) != nullptr)
            ? GST_OBJECT_NAME(src)
            : "?";
        const std::string text =
            (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                          : "unknown";
        log("decode error from " + who + ": " + text
            + (dbg != nullptr ? (" -- " + std::string {dbg}) : std::string {})
            + "\n");
        announced_failure = true;
      }
      m_failed.store(true, std::memory_order_relaxed);
      if (gerr != nullptr) {
        g_error_free(gerr);
      }
      g_free(dbg);
      gst_message_unref(msg);
    }

    // 2) feed: ts_ring -> dsrc, draining everything immediately available.
    for (;;) {
      const std::size_t got = m_ring.read(
          m_consumer, tsbuf.data(), tsbuf.size(), k_read_timeout);
      if (got == 0) {
        break;
      }
      total_fed += got;
      if (!announced_first_feed) {
        log("first TS chunk fed to the decode stage: " + std::to_string(got)
            + " bytes\n");
        announced_first_feed = true;
      }
      GstBuffer* gbuf = gst_buffer_new_allocate(nullptr, got, nullptr);
      gst_buffer_fill(gbuf, 0, tsbuf.data(), got);
      if (gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), gbuf) != GST_FLOW_OK) {
        break;
      }
    }

    // 3) publish one decoded frame (bounded poll keeps the loop responsive).
    GstSample* sample = gst_app_sink_try_pull_sample(
        GST_APP_SINK(m_appsink), k_pull_timeout);
    if (sample != nullptr) {
      publish_sample(sample);
      gst_sample_unref(sample);
    }
  }
}
