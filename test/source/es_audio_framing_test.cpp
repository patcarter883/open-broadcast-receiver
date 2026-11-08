// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// es_audio_framing_test — the audio-framing contract between the standby stage
// and a standby-enabled output.
//
// Why this test exists. The stage parses the ingest once and publishes audio
// access units into the audio es_ring; every output declares the caps of the
// appsrc it feeds those AUs into. Those two statements are the same fact — the
// FRAMING of the bytes in the ring — and when they disagreed nothing failed
// loudly: aacparse was handed buffers with no declared framing, sniffed a
// header on every one, never set src caps, flvmux's audio pad stayed linked but
// starved, the pipeline never prerolled, and the RTMP publish silently timed
// out on every standby session (/stats: audio_frames 0). Only the end-to-end
// harness saw it.
//
// So this test asserts, WITHOUT the harness:
//
//   1. the stage's audio branch declaration and the output's ES audio appsrc
//      declaration name the SAME framing (same stream-format, parsed, version)
//      — a future change to either side fails here instead of killing
//      publishing;
//   2. the framing so declared is self-describing (ADTS), because a ring of raw
//      AAC would need every consumer told the AudioSpecificConfig the stage
//      already knew;
//   3. the contract is executable: audio AFUs produced at the stage's declared
//      framing, pushed into an appsrc declared with the output's caps and run
//      through the output's own branch tail, still reach the muxer's side as
//      raw AAC with an AudioSpecificConfig. That is the chain that was starved.

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "fanout/output.h"
#include "fanout/ring.h"
#include "standby/stage.h"

namespace {
auto expect(bool cond, const std::string &what) -> void {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
  }
}

auto skip(const std::string &why) -> int {
  std::fprintf(stderr, "SKIP: %s\n", why.c_str());
  return 77;
}

auto have_element(const char *name) -> bool {
  GstElementFactory *factory = gst_element_factory_find(name);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

auto trim(std::string s) -> std::string {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  std::size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
    ++start;
  }
  return s.substr(start);
}

// The caps token that follows `marker` in a pipeline description: skip spaces
// and the link operator, then take everything up to the next `!`.
auto caps_after(const std::string &text, std::string_view marker)
    -> std::string {
  const std::size_t at = text.find(marker);
  if (at == std::string::npos) {
    return {};
  }
  std::size_t pos = at + marker.size();
  while (pos < text.size() && text[pos] == ' ') {
    ++pos;
  }
  if (pos < text.size() && text[pos] == '!') {
    ++pos;
  }
  const std::size_t end = text.find('!', pos);
  return trim(text.substr(pos, end == std::string::npos ? end : end - pos));
}

// The `caps=` property that follows `marker` in an element description.
auto caps_property_after(const std::string &text, std::string_view marker)
    -> std::string {
  const std::size_t at = text.find(marker);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t key = text.find("caps=", at + marker.size());
  if (key == std::string::npos) {
    return {};
  }
  const std::size_t start = key + std::string_view{"caps="}.size();
  // A caps value has no spaces: the description ends it at the next blank or
  // link operator, and element properties may follow it.
  const std::size_t end = text.find_first_of(" !", start);
  return trim(text.substr(start, end == std::string::npos ? end : end - start));
}

// One property of a caps string ("audio/mpeg,a=b,c=d" -> "b" -> "1").
auto caps_field(const std::string &caps, std::string_view key) -> std::string {
  const std::string needle = std::string(key) + "=";
  const std::size_t at = caps.find(needle);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + needle.size();
  const std::size_t end = caps.find_first_of(", ", start);
  return caps.substr(start, end == std::string::npos ? end : end - start);
}

struct frame {
  std::vector<unsigned char> bytes;
  GstClockTime pts = 0;
};

// Encode AAC and hand it out in whatever framing `caps` asks for: a stand-in
// for the stage that honours exactly what the stage declares.
auto produce_frames(const std::string &caps, std::vector<frame> &out) -> bool {
  const std::string desc =
      "audiotestsrc is-live=false num-buffers=60 samplesperbuffer=1024 "
      "wave=sine "
      "! audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=64000 "
      "! aacparse ! capsfilter caps=" +
      caps +
      " "
      "! appsink name=psink sync=false";
  GError *err = nullptr;
  GstElement *pipeline = gst_parse_launch(desc.c_str(), &err);
  if (pipeline == nullptr || err != nullptr) {
    std::fprintf(stderr, "producer pipeline: %s\n",
                 err != nullptr ? err->message : "parse failure");
    g_clear_error(&err);
    return false;
  }
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "psink");
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  for (int i = 0; i < 10; ++i) {
    GstSample *sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
    if (sample == nullptr) {
      break;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      frame f;
      f.bytes.assign(map.data, map.data + map.size);
      f.pts = GST_BUFFER_PTS(buffer);
      out.push_back(std::move(f));
      gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  return !out.empty();
}

// Push frames into an appsrc declared with `appsrc_caps`, through the output's
// branch (`aacparse` then the muxer-side capsfilter), and return the caps the
// muxer side negotiated. Empty means nothing reached the muxer at all — the
// starvation this test exists to catch.
auto consume_frames(const std::string &appsrc_caps, const std::string &mux_caps,
                    const std::vector<frame> &frames) -> std::string {
  const std::string desc =
      "appsrc name=cin format=time is-live=false block=false "
      "caps=" +
      appsrc_caps +
      " "
      "! queue ! aacparse name=cparse ! capsfilter name=cmux caps=" +
      mux_caps + " ! appsink name=csink sync=false";
  GError *err = nullptr;
  GstElement *pipeline = gst_parse_launch(desc.c_str(), &err);
  if (pipeline == nullptr || err != nullptr) {
    std::fprintf(stderr, "consumer pipeline: %s\n",
                 err != nullptr ? err->message : "parse failure");
    g_clear_error(&err);
    return {};
  }
  GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "cin");
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "csink");
  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  // The product's feed thread stamps every AU it pushes; appsrc in time format
  // will not carry an unstamped buffer, so a synthetic monotonic timeline here
  // keeps the harness honest about the ONE thing under test (framing).
  GstClockTime pts = 0;
  constexpr GstClockTime k_step = 21'333'333; // one 1024-sample AAC frame
  for (const frame &f : frames) {
    GstBuffer *buffer =
        gst_buffer_new_allocate(nullptr, f.bytes.size(), nullptr);
    gst_buffer_fill(buffer, 0, f.bytes.data(), f.bytes.size());
    GST_BUFFER_PTS(buffer) = pts;
    pts += k_step;
    gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
  }

  std::string negotiated;
  int received = 0;
  int misses = 0;
  for (int i = 0; i < 40; ++i) {
    GstSample *sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 200 * GST_MSECOND);
    if (sample == nullptr) {
      // aacparse holds the final AU until it is flushed; a couple of quiet
      // pulls is the end of the stream, not a stalled branch.
      if (++misses >= 3) {
        break;
      }
      continue;
    }
    misses = 0;
    if (received == 0) {
      GstCaps *caps = gst_sample_get_caps(sample);
      gchar *text = caps != nullptr ? gst_caps_to_string(caps) : nullptr;
      negotiated = text != nullptr ? text : "";
      g_free(text);
    }
    ++received;
    gst_sample_unref(sample);
    if (received >= static_cast<int>(frames.size())) {
      break;
    }
  }

  GstBus *bus = gst_element_get_bus(pipeline);
  while (GstMessage *msg = gst_bus_pop_filtered(
             bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                              GST_MESSAGE_WARNING))) {
    GError *gerr = nullptr;
    gchar *dbg = nullptr;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      gst_message_parse_error(msg, &gerr, &dbg);
      std::fprintf(stderr, "consumer error: %s | %s\n",
                   gerr != nullptr ? gerr->message : "?",
                   dbg != nullptr ? dbg : "");
    } else {
      gst_message_parse_warning(msg, &gerr, &dbg);
      std::fprintf(stderr, "consumer warning: %s | %s\n",
                   gerr != nullptr ? gerr->message : "?",
                   dbg != nullptr ? dbg : "");
    }
    g_clear_error(&gerr);
    g_free(dbg);
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(src);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  return received > 0 ? negotiated : std::string{};
}

auto require(const std::string &caps, std::string_view key,
             std::string_view want, const std::string &where) -> void {
  const std::string got = caps_field(caps, key);
  expect(got == want, std::format("{}: {}={} (want {})", where, key,
                                  got.empty() ? "(unset)" : got, want));
}
} // namespace

auto main() -> int {
  gst_init(nullptr, nullptr);

  for (const char *element : {"aacparse", "avenc_aac", "audiotestsrc"}) {
    if (!have_element(element)) {
      return skip(std::string("missing element ") + element);
    }
  }

  // ---- 1. what the STAGE says it publishes into the audio ring ------------
  es_ring video_ring{es_ring::size_for_bps(1500)};
  es_ring audio_ring{es_ring::size_for_bps(192)};
  ts_ring input_ring{ts_ring::size_for_bandwidth(2000)};
  expect(video_ring.add_consumer() >= 0 && audio_ring.add_consumer() >= 0,
         "consumer slots on both rings");

  standby::stage st{[](const std::string &line) {
    std::fprintf(stderr, "%s\n", line.c_str());
  }};
  standby::stage_config cfg;
  cfg.in_codec = "h264";
  cfg.width = 1280;
  cfg.height = 720;
  cfg.fps = 30;
  cfg.max_gap_ms = 4000;
  cfg.video_ring = &video_ring;
  cfg.audio_ring = &audio_ring;
  cfg.input = &input_ring;

  const bool started = st.start(cfg);
  const std::string description = st.pipeline_description();
  st.stop();
  expect(started, "the stage starts (no media needed to build its branches)");

  const std::string stage_caps =
      caps_after(description, "queue name=aq ! aacparse");
  expect(!stage_caps.empty(),
         "the stage's live audio branch declares caps after aacparse");
  expect(stage_caps.find("audio/mpeg") != std::string::npos,
         "the stage's audio ring carries audio/mpeg: " + stage_caps);

  // ---- 2. what the OUTPUT says it consumes from the audio ring ------------
  es_input_spec es;
  es.enabled = true;
  es.video_codec = codec::h264;
  es.audio = true;
  const transcode_config copy{}; // no transcode: the copy/remux template
  const std::string tmpl =
      output_template(output_proto::rtmp, copy, nullptr, es);
  const std::string output_caps = caps_property_after(tmpl, "appsrc name=asrc");
  expect(output_caps.find("audio/mpeg") != std::string::npos,
         "the ES audio appsrc declares audio/mpeg caps: " + output_caps);

  // The muxer's own requirement, stated by the branch tail: raw AAC.
  const std::string mux_caps =
      caps_property_after(tmpl, "capsfilter name=acaps");
  require(mux_caps, "stream-format", "raw", "the muxer-side capsfilter");

  // ---- 3. the two declarations must be the SAME framing -------------------
  for (const char *key : {"mpegversion", "stream-format", "parsed"}) {
    const std::string stage_value = caps_field(stage_caps, key);
    const std::string output_value = caps_field(output_caps, key);
    expect(!stage_value.empty(),
           std::format("the stage states {} in the ring's framing", key));
    expect(!output_value.empty(),
           std::format("the output states {} instead of guessing it", key));
    expect(stage_value == output_value,
           std::format("stage and output agree on {} ({} vs {})", key,
                       stage_value, output_value));
  }
  // Self-describing or not is the whole point: raw AAC in a ring cannot be
  // parsed by a consumer that was not handed the codec's configuration.
  require(stage_caps, "stream-format", "adts", "the stage's audio ring");
  require(output_caps, "stream-format", "adts", "the ES audio appsrc");

  // ---- 4. ...and the contract is executable ------------------------------
  std::vector<frame> frames;
  expect(produce_frames(stage_caps, frames),
         "AAC can be produced at the framing the stage declares (" +
             stage_caps + ")");
  const std::string negotiated = consume_frames(output_caps, mux_caps, frames);
  expect(!negotiated.empty(),
         "audio at the stage's framing negotiates through the output's ES "
         "audio appsrc caps (" +
             output_caps +
             "): aacparse set src caps and handed the muxer audio");
  expect(negotiated.find("stream-format=(string)raw") != std::string::npos,
         "the output's branch reaches raw AAC at the muxer: " + negotiated);
  expect(negotiated.find("codec_data") != std::string::npos,
         "the muxer-side caps carry the AudioSpecificConfig flvmux writes: " +
             negotiated);

  std::fprintf(stderr, "PASS: stage and output agree on ADTS audio framing\n");
  return 0;
}
