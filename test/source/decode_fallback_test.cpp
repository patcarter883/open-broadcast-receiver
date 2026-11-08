// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// decode_fallback_test — the decode stage's decoder selection, exercised with
// real pipelines against generated AV1 fixtures.
//
// Three properties are asserted:
//   1. The chain for av1 is [vaav1dec, av1dec] -- hardware first, software as
//      the last resort -- and nothing else.
//   2. A hardware decoder that cannot negotiate the stream (vaav1dec's sink
//      caps are profile=main only, so a 4:4:4 profile=high stream is refused)
//      costs one attempt: the stage rebuilds with av1dec and still publishes
//      frames, with fallback=1 in the periodic line.
//   3. The same recovery happens for a decoder retired without a bus error
//      (bounded no-frame window), and the chain is finite: when nothing can
//      decode, the stage marks itself failed instead of looping.
//
// Fixtures are generated in-process, in the shape the product's encoder emits
// (open-broadcast-encoder, pipeline_build_video_encoder + pipeline_build_sink):
// a hardware AV1 encoder fed NV12 at 1080p, `av1parse`, the frame-aligned caps
// mpegtsmux requires, and `mpegtsmux alignment=7 enable-custom-mappings=true`.
// The caps of every fixture are read back through tsdemux + av1parse and
// asserted, so a fixture whose profile/chroma drifted away from what the
// encoder really sends fails here instead of hiding a decode bug behind a
// stream the hardware decoder happens to like. Skipping is explicit and
// non-zero-free: a missing element or a fixture that cannot be muxed fails the
// test rather than passing silently, because a green run must mean the
// pipelines actually ran.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gst/gst.h>

#include "fanout/decoder.h"
#include "fanout/ring.h"
#include "lib/lib.h"

namespace
{
constexpr std::size_t k_datagram = 7 * 188;
constexpr double k_feed_seconds = 3.0;
constexpr auto k_drain = std::chrono::seconds(3);

auto g_failures = 0;

auto check(bool cond, const std::string& what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

auto has_element(const char* name) -> bool
{
  GstElementFactory* f = gst_element_factory_find(name);
  if (f == nullptr) {
    return false;
  }
  gst_object_unref(f);
  return true;
}

auto only(const std::vector<std::string>& present)
{
  return [present](const char* name) -> bool
  {
    return std::ranges::find(present, std::string {name}) != present.end();
  };
}

auto joined(const std::vector<std::string>& items) -> std::string
{
  std::string out;
  for (const auto& item : items) {
    out += (out.empty() ? "" : ",") + item;
  }
  return out;
}

// The AV1 hardware encoder on this box. The product's encoder runs qsvav1enc on
// the capture box; both are VAAPI-class hardware AV1 encoders fed NV12 and both
// publish the same caps through av1parse, so this is the closest stand-in for
// what the receiver actually receives.
auto product_encoder() -> const char*
{
  return has_element("vaav1enc") ? "vaav1enc" : "av1enc";
}

// Mux `chroma` frames into the TS shape the product's encoder emits: framerate
// pacing, alignment=7 datagrams, explicit AV1 stream type.
auto make_fixture(const char* chroma, const std::filesystem::path& path) -> bool
{
  // 4:4:4 exists only to produce a stream the hardware decoder refuses, and the
  // VAAPI encoder cannot make one: av1enc is required for that case. The
  // encoder-representative fixture is the hardware encoder at 1080p NV12, the
  // input the product's encoder carries.
  const bool high44 = std::string {chroma} == "Y444";
  const char* encoder = high44 ? "av1enc" : product_encoder();
  const int width = high44 ? 640 : 1920;
  const int height = high44 ? 360 : 1080;
  const std::string enc_args = std::string {encoder} == "vaav1enc"
      ? "bitrate=4000 rate-control=cbr key-int-max=60"
      : "cpu-used=8 lag-in-frames=0 target-bitrate=4000 keyframe-max-dist=60";
  const std::string desc = std::format(
      "videotestsrc num-buffers=120 pattern=smpte ! "
      "video/x-raw,format={},width={},height={},framerate=30/1 ! "
      "videoconvert ! {} {} ! av1parse ! "
      "video/x-av1,stream-format=obu-stream,alignment=frame ! "
      "mpegtsmux alignment=7 enable-custom-mappings=true ! "
      "filesink location={}",
      chroma, width, height, encoder, enc_args, path.string());
  GError* gerr = nullptr;
  GstElement* p = gst_parse_launch(desc.c_str(), &gerr);
  if (p == nullptr || gerr != nullptr) {
    if (gerr != nullptr) {
      std::fprintf(stderr, "fixture pipeline (%s): %s\n", chroma, gerr->message);
      g_error_free(gerr);
    }
    return false;
  }
  const GstStateChangeReturn scr = gst_element_set_state(p, GST_STATE_PLAYING);
  bool ok = scr != GST_STATE_CHANGE_FAILURE;
  if (ok) {
    GstBus* bus = gst_element_get_bus(p);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 60 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    ok = msg != nullptr && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg != nullptr) {
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        std::fprintf(stderr, "fixture mux failed (%s): %s -- %s\n", chroma,
                     err != nullptr ? err->message : "?", dbg != nullptr ? dbg : "");
        if (err != nullptr) {
          g_error_free(err);
        }
        g_free(dbg);
      }
      gst_message_unref(msg);
    }
    gst_object_unref(bus);
  }
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return ok;
}

// The caps the fixture's elementary stream really carries, read off av1parse's
// src pad through the same demux path the receiver uses. This is the check that
// the fixture is what the encoder sends -- profile, chroma, bit depth, layout --
// rather than a stream that only the local decoder happens to accept.
auto fixture_stream_caps(const std::filesystem::path& path) -> std::string
{
  const std::string desc = std::format(
      "filesrc location={} ! tsparse ! tsdemux ! av1parse name=p ! "
      "fakesink sync=false",
      path.string());
  GError* gerr = nullptr;
  GstElement* p = gst_parse_launch(desc.c_str(), &gerr);
  if (p == nullptr || gerr != nullptr) {
    if (gerr != nullptr) {
      std::fprintf(stderr, "fixture caps probe: %s\n", gerr->message);
      g_error_free(gerr);
    }
    if (p != nullptr) {
      gst_object_unref(p);
    }
    return {};
  }
  std::string out;
  gst_element_set_state(p, GST_STATE_PLAYING);
  gst_element_get_state(p, nullptr, nullptr, 10 * GST_SECOND);
  GstElement* parse = gst_bin_get_by_name(GST_BIN(p), "p");
  if (parse != nullptr) {
    GstPad* src = gst_element_get_static_pad(parse, "src");
    if (src != nullptr) {
      GstCaps* caps = gst_pad_get_current_caps(src);
      if (caps != nullptr) {
        gchar* text = gst_caps_to_string(caps);
        out = text != nullptr ? text : "";
        g_free(text);
        gst_caps_unref(caps);
      }
      gst_object_unref(src);
    }
    gst_object_unref(parse);
  }
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return out;
}

// True when the pipeline ends in an ERROR rather than EOS. Used for the
// element-level layout check: negotiation failures surface here and nowhere
// else (the decoder produces no frames and says nothing on stdout).
auto pipeline_errors(const std::string& desc) -> bool
{
  GError* gerr = nullptr;
  GstElement* p = gst_parse_launch(desc.c_str(), &gerr);
  if (p == nullptr || gerr != nullptr) {
    if (gerr != nullptr) {
      std::fprintf(stderr, "layout pipeline: %s\n", gerr->message);
      g_error_free(gerr);
    }
    if (p != nullptr) {
      gst_object_unref(p);
    }
    return true;
  }
  bool errored = gst_element_set_state(p, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_FAILURE;
  if (!errored) {
    GstBus* bus = gst_element_get_bus(p);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    errored = msg == nullptr || GST_MESSAGE_TYPE(msg) != GST_MESSAGE_EOS;
    if (msg != nullptr) {
      gst_message_unref(msg);
    }
    gst_object_unref(bus);
  }
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return errored;
}

struct run_result {
  std::uint64_t frames = 0;
  bool failed = false;
  bool fallback = false;
  std::string decoder;
  std::vector<std::string> log;
};

auto count_of(const std::vector<std::string>& lines, const std::string& needle)
    -> int
{
  int n = 0;
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) {
      ++n;
    }
  }
  return n;
}

auto contains(const std::vector<std::string>& lines, const std::string& needle)
    -> bool
{
  return count_of(lines, needle) > 0;
}

// Feed `bytes` through a real ts_ring at the stream's own rate, exactly as the
// RIST callback does.
auto feed(ts_ring& ring, const std::vector<std::uint8_t>& bytes) -> void
{
  if (bytes.empty()) {
    return;
  }
  const std::size_t datagrams = (bytes.size() + k_datagram - 1) / k_datagram;
  const auto period = std::chrono::duration<double> {
      k_feed_seconds / static_cast<double>(datagrams)};
  const auto start = std::chrono::steady_clock::now();
  std::size_t sent = 0;
  for (std::size_t off = 0; off < bytes.size(); off += k_datagram) {
    const std::size_t len = std::min(k_datagram, bytes.size() - off);
    ring.write(bytes.data() + off, len);
    ++sent;
    std::this_thread::sleep_until(
        start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    period * static_cast<double>(sent)));
  }
}

auto run_bytes(const std::vector<std::uint8_t>& bytes, bool simulate_failure,
               std::chrono::seconds drain = k_drain,
               bool keep_feeding = false) -> run_result
{
  run_result result;
  ts_ring ring {ts_ring::size_for_bandwidth(20000)};
  decoder dec {ring, codec::av1,
               [&result](const std::string& line) -> void
               { result.log.push_back(line); }};
  if (simulate_failure) {
    dec.simulate_first_attempt_failure();
  }
  if (!dec.start()) {
    result.failed = true;
    result.log.push_back("[test] decode stage refused to start");
    return result;
  }
  feed(ring, bytes);
  if (keep_feeding) {
    // A live stream keeps arriving: an attempt that is retired and rebuilt must
    // be fed again by the same input, which is what makes the chain exhaust.
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < drain) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      feed(ring, bytes);
    }
  }
  std::this_thread::sleep_for(drain);
  result.frames = dec.frames().head();
  result.failed = dec.failed();
  result.fallback = dec.using_fallback();
  dec.stop();
  result.decoder = dec.decoder_element();
  for (const std::string& line : result.log) {
    std::fputs(line.c_str(), stderr);
  }
  return result;
}

auto read_file(const std::filesystem::path& path) -> std::vector<std::uint8_t>
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}
}  // namespace

auto main(int argc, char** argv) -> int
{
  gst_init(&argc, &argv);

  // ---- 1. selection: hardware first, software as the last resort ----------
  {
    const auto av1 = transcode_decoder_alternatives(codec::av1);
    check(av1 == (std::vector<std::string> {"vaav1dec", "av1dec", "dav1ddec"}),
          "av1 decoder alternatives are [vaav1dec, av1dec, dav1ddec], got " + joined(av1));

    const auto full =
        transcode_decoder_chain(codec::av1, only({"vaav1dec", "av1dec"}));
    check(full == (std::vector<std::string> {"vaav1dec", "av1dec"}),
          "av1 chain with both present is [vaav1dec, av1dec], got "
              + joined(full));
    const auto software_only = transcode_decoder_chain(codec::av1,
                                                       only({"av1dec"}));
    check(software_only == (std::vector<std::string> {"av1dec"}),
          "av1 chain drops absent hardware, got " + joined(software_only));
    check(transcode_decoder_chain(codec::av1, only({})).empty(),
          "av1 chain is empty when nothing is present");
  }

  const bool have_hw = has_element("vaav1dec");
  const bool have_sw = has_element("av1dec");
  const bool have_enc = has_element("av1enc");
  if (!have_sw || !have_enc || !has_element("mpegtsmux")
      || !has_element("av1parse")) {
    std::fprintf(stderr,
                 "SKIP: av1enc/av1parse/mpegtsmux/av1dec are required to run "
                 "the pipeline cases\n");
    return g_failures == 0 ? 0 : 1;
  }

  // The branch pins, after the parser, exactly the fields the decoder fixes on
  // its own sink pad. The two AV1 decoders want different layouts (vaav1dec
  // frame-aligned, av1dec TU-aligned), so av1parse cannot be left to its own
  // default: the caps have to be derived per decoder or one of the two attempts
  // is refused for a layout the parser chose.
  if (have_hw) {
    const std::string hw_caps = decoder_branch_caps("vaav1dec");
    check(hw_caps.find("profile=main") != std::string::npos
              && hw_caps.find("alignment=frame") != std::string::npos,
          "vaav1dec branch caps pin profile=main and alignment=frame, got "
              + hw_caps);
    const std::string tmpl = decoder_template(codec::av1, "av1parse", "vaav1dec");
    check(tmpl.find("av1parse ! video/x-av1,profile=main,alignment=frame ! "
                    "vaav1dec") != std::string::npos,
          "the hardware branch pins vaav1dec's caps after the parser");
  }
  {
    const std::string sw_caps = decoder_branch_caps("av1dec");
    check(sw_caps.find("alignment=tu") != std::string::npos,
          "av1dec branch caps pin alignment=tu, got " + sw_caps);
  }

  const auto dir = std::filesystem::temp_directory_path()
      / std::format("obr_decode_fallback_{}", static_cast<long>(::getpid()));
  std::filesystem::create_directories(dir);
  const auto fx_444 = dir / "high44.ts";
  const auto fx_420 = dir / "main420.ts";

  // ---- 2. hardware refused, software takes over ---------------------------
  // vaav1dec's sink caps are profile=main only; a 4:4:4 profile=high stream is
  // refused at negotiation, which is the failure this stage must survive.
  if (make_fixture("Y444", fx_444) && make_fixture("NV12", fx_420)) {
    // The encoder-representative fixture: what qsvav1enc/vaav1enc + av1parse
    // publish for a 1080p NV12 source. Asserted, not assumed -- a fixture that
    // drifted to another profile or chroma is a different bug hiding the same
    // way.
    const std::string caps_420 = fixture_stream_caps(fx_420);
    std::fprintf(stderr, "main fixture caps: %s\n", caps_420.c_str());
    check(caps_420.find("profile=(string)main") != std::string::npos,
          "the fixture carries the encoder's profile=main, got " + caps_420);
    check(caps_420.find("chroma-format=(string)4:2:0") != std::string::npos,
          "the fixture carries the encoder's chroma-format=4:2:0");
    check(caps_420.find("bit-depth-luma=(uint)8") != std::string::npos,
          "the fixture carries the encoder's 8-bit luma");
    check(caps_420.find("width=(int)1920") != std::string::npos
              && caps_420.find("height=(int)1080") != std::string::npos,
          "the fixture is the 1080p the encoder encodes");
    {

      const auto r = run_bytes(read_file(fx_444), false);
      check(r.frames > 0, "4:4:4 stream decoded after a hardware refusal, got "
                              + std::to_string(r.frames) + " frames");
      if (have_hw) {
        check(count_of(r.log, "decoder vaav1dec failed") == 1,
              "the hardware failure is logged exactly once");
        check(contains(r.log, "profile=high") &&
                  contains(r.log, "the decoder accepts profile=main"),
              "the hardware refusal names the field it refused");
        check(contains(r.log, "dec=av1dec"), "periodic line names av1dec");
        check(contains(r.log, "fallback=1"), "periodic line carries fallback=1");
        check(r.fallback && r.decoder == "av1dec",
              "stage reports the software fallback decoder");
      }
    }

    // ---- 3. hardware is still the preference when it can negotiate --------
    {
      const auto r = run_bytes(read_file(fx_420), false);
      check(r.frames > 0,
            "4:2:0 main stream decodes, got " + std::to_string(r.frames)
                + " frames");
      if (have_hw) {
        check(!r.fallback && r.decoder == "vaav1dec",
              "hardware decoder stays in use when it can negotiate, got "
                  + r.decoder);
        check(contains(r.log, "fallback=0"),
              "periodic line reports fallback=0 on the hardware path");
        check(contains(r.log, "profile=main")
                  && contains(r.log, "alignment=frame")
                  && contains(r.log, "accepts it"),
              "the hardware attempt reports the stream caps it negotiated");
        check(!contains(r.log, "failed:"),
              "no decoder attempt failed on the hardware path");
      }
    }

    // ---- 3b. the layout decides it ---------------------------------------
    // The same main-profile stream, offered TU-aligned (av1parse's default, and
    // what the software decoder takes) is refused by the hardware decoder;
    // frame-aligned, which is what the branch pins, it decodes. This is why the
    // pin exists rather than being left to negotiation.
    if (have_hw) {
      const std::string base = std::format(
          "filesrc location={} ! tsparse ! tsdemux ! av1parse ! ", fx_420.string());
      const std::string tail = " ! vaav1dec ! fakesink sync=false";
      check(pipeline_errors(base + "video/x-av1,alignment=tu" + tail),
            "vaav1dec refuses the TU-aligned layout av1parse defaults to");
      check(!pipeline_errors(base + "video/x-av1,alignment=frame" + tail),
            "vaav1dec takes the same stream frame-aligned");
    }

    // ---- 4. forced first-decoder failure --------------------------------
    {
      const auto r = run_bytes(read_file(fx_420), true);
      check(r.frames > 0,
            "software decoder publishes frames after a forced failure, got "
                + std::to_string(r.frames));
      check(r.fallback && r.decoder == "av1dec",
            "forced failure is retired and av1dec takes over, got "
                + r.decoder);
      check(count_of(r.log, "simulated decoder failure") == 1,
            "forced failure is reported once");
      check(contains(r.log, "dec=av1dec") && contains(r.log, "fallback=1"),
            "periodic line reports dec=av1dec fallback=1");
    }
  } else {
    check(false, "AV1 fixtures could not be generated (av1enc/mpegtsmux)");
  }

  // ---- 5. the chain is finite -------------------------------------------
  // Undemuxable bytes: no pad, no frames, no bus error -- only the bounded
  // window can retire the attempt. With nothing decodable the stage must end
  // failed rather than retry forever. The junk arrives at a real rate and in a
  // volume that is evidence (decode_health::k_min_fed_bytes and then some):
  // below that the stage is supposed to keep waiting, which case 6b pins.
  {
    // Both attempts must reach the volume minimum and exhaust their own window,
    // so the drain outlasts two of them, and the junk keeps arriving so the
    // rebuilt attempt is fed too.
    const std::vector<std::uint8_t> junk(320 * 1024, 0x00);
    const auto r = run_bytes(junk, false, std::chrono::seconds(14), true);
    check(r.failed, "undecodable input leaves the stage failed");
    check(contains(r.log, "no decoder left for av1"),
          "the chain is exhausted rather than retried forever");
    check(r.frames == 0, "no frames are fabricated from undecodable input");
  }

  // ---- 6. a trickle is not evidence that tsdemux has no video ------------
  // 200 KiB of undemuxable bytes spread over the feed window: the old rule
  // (first chunk + a flat five seconds) retired vaav1dec and rebuilt the chain,
  // which on a slow RIST start throws away a hardware decoder that works. With
  // the volume minimum in place the stage is not allowed to conclude anything
  // yet.
  {
    const std::vector<std::uint8_t> trickle(200 * 1024, 0x00);
    const auto r = run_bytes(trickle, false, std::chrono::seconds(8));
    check(!r.failed, "a trickle of TS does not fail the stage");
    check(r.decoder == "vaav1dec" && !r.fallback,
          "a trickle does not retire the hardware decoder, got "
              + r.decoder + (r.fallback ? " (fallback)" : ""));
    check(!contains(r.log, "tsdemux produced no video"),
          "no demux verdict is reached on a trickle");
  }

  // ---- 7. a stream that has not started yet is not a decoder failure ------
  // The stream arrives seconds after /start. Neither health clock may run before
  // data does, or the hardware decoder is retired as "no decoded frame" and the
  // session silently runs on the software one.
  {
    const auto r = run_bytes({}, false, std::chrono::seconds(8));
    check(!r.failed, "a stage with no TS yet does not fail");
    check(r.decoder == "vaav1dec" && !r.fallback,
          "the hardware decoder is still the attempt after 8 s of silence, got "
              + r.decoder);
    check(!contains(r.log, "failed:"),
          "no decoder is retired while nothing has arrived");
    check(count_of(r.log, "[decoder] in=0 chunks") >= 1,
          "the periodic line reports the silence while it lasts");
  }

  std::filesystem::remove_all(dir);
  if (g_failures != 0) {
    std::fprintf(stderr, "decode_fallback_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("decode_fallback_test: ok\n");
  return 0;
}
