// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// decode_file — run the receiver's OWN decode stage against a local MPEG-TS
// file, with no RIST, no network and no outputs.
//
// The tool builds the exact pipeline `decoder_template()` produces — the same
// code path the receiver uses, not a copy — feeds a fixture through a real
// ts_ring at RIST datagram granularity, and reports the stage's counters. With
// --expect-frames it asserts on them, which is how CTest runs it
// (test/tools/run_decode_fixture.sh).
//
// The fixture MUST be muxed the way the product's encoder muxes -- and there is
// exactly one supported way to produce one, so use the committed generator
// rather than an ad-hoc pipeline:
//
//   make fixture              # -> build/av1-fixture.ts
//   ./build/test/decode_file build/av1-fixture.ts av1
//
// test/tools/make-av1-ts-fixture.sh encodes 150 frames (5 s) with the
// deployment's AV1 encoder and muxes with
//
//   av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame
//     ! mpegtsmux alignment=7 enable-custom-mappings=true
//
// av1parse is required: encoders emit alignment=tu and mpegtsmux only accepts
// alignment=frame; the encoder's own chain has the same av1parse (encode.cpp).
// `enable-custom-mappings=true` is the one that matters most: MPEG-TS has no
// standardised AV1 stream type, so a TS muxed without it has no AV1 stream type
// and tsdemux exposes no video/x-av1 pad ("This stream contains no valid or
// supported streams") -- a fixture like that proves nothing about the decoder.
// alignment=7 = 1316-byte buffers = the RIST datagram the receiver feeds.
//
// Usage: decode_file <fixture.ts> [codec] [--expect-frames N] [--duration S]
//   codec: av1 (default) | h264 | h265
// Exit 0 when the stage ran (and met --expect-frames); 1 when an assertion
// failed; 2 on a usage or fixture error.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "fanout/decoder.h"
#include "fanout/ring.h"

namespace
{
// RIST carries MPEG-TS in datagrams of 7 TS packets by default, which is also
// what the encoder's mpegtsmux alignment=7 produces.
constexpr std::size_t k_datagram = 7 * 188;
constexpr auto k_drain = std::chrono::seconds(2);
// Feeding a fixture in one burst is not what a live ingest does: the decode
// loop drains the appsink in bounded bursts, so a burst feed overflows its
// 8-buffer queue and the measurement becomes a statement about the harness.
// Pacing the datagrams over the fixture's real duration reproduces the live
// arrival rate. --duration overrides it.
constexpr double k_default_seconds = 5.0;
// A live stage can lose a frame or two at the head while it prerolls and at the
// tail while the drain finishes; the counters are asserted against this fraction
// of the fixture's frame count, which still catches a stage that publishes none.
constexpr double k_published_fraction = 0.8;
}  // namespace

auto main(int argc, char** argv) -> int
{
  std::string fixture;
  codec in_codec = codec::av1;
  long expect_frames = -1;
  double duration = k_default_seconds;
  bool codec_given = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg {argv[i]};
    if (arg == "--expect-frames") {
      if (i + 1 >= argc) {
        fprintf(stderr, "--expect-frames needs a value\n");
        return 2;
      }
      expect_frames = std::strtol(argv[++i], nullptr, 10);
      continue;
    }
    if (arg == "--duration") {
      if (i + 1 >= argc) {
        fprintf(stderr, "--duration needs a value\n");
        return 2;
      }
      duration = std::strtod(argv[++i], nullptr);
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      fprintf(stderr, "unknown option %s\n", arg.c_str());
      return 2;
    }
    if (fixture.empty()) {
      fixture = arg;
    } else if (!codec_given) {
      codec_given = true;
      if (arg == "h264") {
        in_codec = codec::h264;
      } else if (arg == "h265") {
        in_codec = codec::h265;
      } else if (arg != "av1") {
        fprintf(stderr, "unknown codec %s\n", arg.c_str());
        return 2;
      }
    } else {
      fprintf(stderr, "unexpected argument %s\n", arg.c_str());
      return 2;
    }
  }

  if (fixture.empty()) {
    fprintf(stderr,
            "usage: %s <fixture.ts> [av1|h264|h265] [--expect-frames N] "
            "[--duration S]\n",
            argv[0]);
    return 2;
  }

  std::ifstream in(fixture, std::ios::binary);
  if (!in) {
    fprintf(stderr, "cannot open %s\n", fixture.c_str());
    return 2;
  }
  const std::vector<std::uint8_t> ts {std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>()};
  if (ts.empty()) {
    fprintf(stderr, "%s is empty\n", fixture.c_str());
    return 2;
  }

  gst_init(&argc, &argv);

  ts_ring ring {ts_ring::size_for_bandwidth(20000)};
  decoder dec {ring, in_codec, [](const std::string& line) -> void {
                 fputs(line.c_str(), stderr);
                 fflush(stderr);
               }};
  if (!dec.start()) {
    fprintf(stderr, "decode stage failed to start\n");
    return 1;
  }

  // Feed exactly as the RIST callback does: fixed-size datagrams arriving at the
  // stream's own rate.
  std::thread feeder {[&ring, &ts, duration]() -> void {
    const std::size_t datagrams = (ts.size() + k_datagram - 1) / k_datagram;
    const auto period = std::chrono::duration<double> {
        duration / static_cast<double>(datagrams)};
    const auto start = std::chrono::steady_clock::now();
    std::size_t sent = 0;
    for (std::size_t off = 0; off < ts.size(); off += k_datagram) {
      const std::size_t len = std::min(k_datagram, ts.size() - off);
      ring.write(ts.data() + off, len);
      ++sent;
      const auto due = start + std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(period * static_cast<double>(sent));
      std::this_thread::sleep_until(due);
    }
  }};
  feeder.join();
  fprintf(stderr, "[decode_file] fed %zu bytes as %zu-byte datagrams\n",
          ts.size(), k_datagram);

  std::this_thread::sleep_for(k_drain);
  const std::uint64_t frames = dec.frames().head();
  const decoder::stage_counters counters = dec.counters();
  dec.stop();

  printf("fixture=%s codec=%s frames=%llu failed=%d demux=%llu dec=%llu "
         "sink=%llu state=%s stalled=%d\n",
         fixture.c_str(),
         in_codec == codec::av1 ? "av1"
         : in_codec == codec::h264 ? "h264"
                                   : "h265",
         static_cast<unsigned long long>(frames), counters.failed ? 1 : 0,
         static_cast<unsigned long long>(counters.demux_buffers),
         static_cast<unsigned long long>(counters.decoder_buffers),
         static_cast<unsigned long long>(counters.sink_buffers),
         gst_element_state_get_name(counters.state), counters.stalled ? 1 : 0);
  fflush(stdout);
  fprintf(stderr,
          "[decode_file] frames published to the frame ring: %llu "
          "failed=%d demux=%llu dec=%llu sink=%llu state=%s stalled=%d\n",
          static_cast<unsigned long long>(frames), counters.failed ? 1 : 0,
          static_cast<unsigned long long>(counters.demux_buffers),
          static_cast<unsigned long long>(counters.decoder_buffers),
          static_cast<unsigned long long>(counters.sink_buffers),
          gst_element_state_get_name(counters.state), counters.stalled ? 1 : 0);

  if (expect_frames < 0) {
    return 0;
  }

  // Assertions. An unlinked drop branch leaves the bin in ASYNC with zero
  // frames decoded, so the state check and the frame floor fail together.
  const auto expected = static_cast<std::uint64_t>(expect_frames);
  const auto floor = static_cast<std::uint64_t>(
      std::ceil(static_cast<double>(expected) * k_published_fraction));
  int failures = 0;
  const auto check = [&failures](bool ok, const char* what) -> void {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  };
  check(counters.state == GST_STATE_PLAYING, "pipeline did not reach PLAYING");
  check(!counters.failed, "decode stage reported an error");
  check(!counters.stalled, "decode stage stalled");
  check(counters.demux_buffers > 0, "tsdemux produced no video buffers");
  check(counters.decoder_buffers > 0, "decoder emitted no frames");
  check(counters.sink_buffers > 0, "appsink received nothing");
  check(frames >= floor,
        "published frame count below the expected floor");
  if (failures != 0) {
    fprintf(stderr,
            "FAIL: %d check(s); frames=%llu floor=%llu of expected=%llu\n",
            failures, static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(floor),
            static_cast<unsigned long long>(expected));
    return 1;
  }
  fprintf(stderr, "PASS: %llu frames published (floor %llu of %llu), state %s\n",
          static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(floor),
          static_cast<unsigned long long>(expected),
          gst_element_state_get_name(counters.state));
  return 0;
}
