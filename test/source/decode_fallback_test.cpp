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
// Fixtures are generated in-process so the test does not depend on a file
// outside the repo; the encoder is av1enc, the same one the product's encoder
// chain uses. Skipping is explicit and non-zero-free: a missing element or a
// fixture that cannot be muxed fails the test rather than passing silently,
// because a green run must mean the pipelines actually ran.

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

// Mux `chroma` frames into the TS shape the product's encoder emits: framerate
// pacing, alignment=7 datagrams, explicit AV1 stream type.
auto make_fixture(const char* chroma, const std::filesystem::path& path) -> bool
{
  const std::string desc = std::format(
      "videotestsrc num-buffers=120 ! "
      "video/x-raw,format={},width=640,height=360,framerate=30/1 ! "
      "av1enc cpu-used=8 lag-in-frames=0 target-bitrate=2000 "
      "keyframe-max-dist=30 ! av1parse ! "
      "video/x-av1,stream-format=obu-stream,alignment=frame ! "
      "mpegtsmux alignment=7 enable-custom-mappings=true ! "
      "filesink location={}",
      chroma, path.string());
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
               std::chrono::seconds drain = k_drain) -> run_result
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

  const auto dir = std::filesystem::temp_directory_path()
      / std::format("obr_decode_fallback_{}", static_cast<long>(::getpid()));
  std::filesystem::create_directories(dir);
  const auto fx_444 = dir / "high44.ts";
  const auto fx_420 = dir / "main420.ts";

  // ---- 2. hardware refused, software takes over ---------------------------
  // vaav1dec's sink caps are profile=main only; a 4:4:4 profile=high stream is
  // refused at negotiation, which is the failure this stage must survive.
  if (make_fixture("Y444", fx_444) && make_fixture("I420", fx_420)) {
    {
      const auto r = run_bytes(read_file(fx_444), false);
      check(r.frames > 0, "4:4:4 stream decoded after a hardware refusal, got "
                              + std::to_string(r.frames) + " frames");
      if (have_hw) {
        check(count_of(r.log, "decoder vaav1dec failed") == 1,
              "the hardware failure is logged exactly once");
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
      }
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
  // failed rather than retry forever.
  {
    // Both attempts must exhaust their own window, so the drain outlasts two
    // of them.
    const std::vector<std::uint8_t> junk(16 * 1024, 0x00);
    const auto r = run_bytes(junk, false, std::chrono::seconds(6));
    check(r.failed, "undecodable input leaves the stage failed");
    check(contains(r.log, "no decoder left for av1"),
          "the chain is exhausted rather than retried forever");
    check(r.frames == 0, "no frames are fabricated from undecodable input");
  }

  std::filesystem::remove_all(dir);
  if (g_failures != 0) {
    std::fprintf(stderr, "decode_fallback_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("decode_fallback_test: ok\n");
  return 0;
}
