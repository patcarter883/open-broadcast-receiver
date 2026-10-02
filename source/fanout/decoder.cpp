// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "fanout/decoder.h"

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
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
}  // namespace

auto decoder_template(codec /*in_codec*/,
                      const char* source_parser,
                      const char* decoder) -> std::string
{
  // The parse element is chosen from the source codec and the decoder from the
  // same alternatives list required_elements() preflights (lib.h), so the
  // runtime can never pick an element the preflight did not check.
  return std::format(
      "appsrc name=dsrc is-live=true do-timestamp=true format=time "
      "block=true max-bytes=4194304 "
      "! tsparse set-timestamps=true alignment=7 "
      "! tsdemux name=d "
      "d. ! queue ! {} ! {} ! videoconvert ! video/x-raw,format=NV12 "
      "! appsink name=dsink sync=false max-buffers=8 drop=true",
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
  if (m_thread.joinable()) {
    m_thread.join();
  }
  destroy_pipeline();
  m_frames.close();  // wake every blocked transcode consumer
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
        const std::string text =
            (gerr != nullptr && gerr->message != nullptr) ? gerr->message
                                                          : "unknown";
        log("decode error: " + text + "\n");
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
