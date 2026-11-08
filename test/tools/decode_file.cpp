// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// decode_file — run the receiver's OWN decode stage against a local MPEG-TS
// file, with no RIST, no network and no outputs.
//
// WHY THIS EXISTS: the decode stage could only be exercised by running the
// whole receiver against a live rig, so a stall in it ("state=3 ASYNC, zero
// frames, no element errors") could not be reproduced off-rig. This tool builds
// the exact pipeline `decoder_template()` produces — the same code path the
// receiver uses, not a copy — feeds a fixture through a real ts_ring at RIST
// datagram granularity, and prints the counters the stage publishes.
//
// The fixture MUST be muxed the way the product's encoder muxes:
//   gst-launch-1.0 -e videotestsrc num-buffers=150 \
//     ! video/x-raw,format=I420,width=1280,height=720,framerate=30/1 \
//     ! av1enc cpu-used=8 lag-in-frames=0 row-mt=true tile-columns=2 \
//       target-bitrate=4000 keyframe-max-dist=30 \
//     ! av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame \
//     ! mpegtsmux alignment=7 enable-custom-mappings=true \
//     ! filesink location=/tmp/av1.ts
// av1parse is required: av1enc emits alignment=tu and mpegtsmux only accepts
// alignment=frame; the encoder's own chain has the same av1parse (encode.cpp).
// format=I420 matters too: any other chroma makes av1enc emit profile=high,
// which vaav1dec's sink caps (profile=main) refuse. A TS muxed without
// enable-custom-mappings has no AV1 stream type and tsdemux exposes no
// video/x-av1 pad for it.
//
// Usage: decode_file <fixture.ts> [codec]
//   codec: av1 (default) | h264 | h265
// Exit 0 when the stage ran; the printed `frames=` line is the measurement.

#include <chrono>
#include <cstdint>
#include <cstdio>
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
// The fixture is 150 frames at 30 fps. Feeding 12 MiB in a single burst is not
// what a live ingest does: the decode loop drains the appsink in bounded bursts,
// so a burst feed overflows its 8-buffer queue and the measurement becomes a
// statement about the harness. Pacing the datagrams over the fixture's real
// duration reproduces the live arrival rate.
constexpr double k_fixture_seconds = 5.0;
}  // namespace

auto main(int argc, char** argv) -> int
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <fixture.ts> [av1|h264|h265]\n", argv[0]);
    return 2;
  }
  codec in_codec = codec::av1;
  if (argc > 2) {
    const std::string name {argv[2]};
    if (name == "h264") {
      in_codec = codec::h264;
    } else if (name == "h265") {
      in_codec = codec::h265;
    } else if (name != "av1") {
      fprintf(stderr, "unknown codec %s\n", name.c_str());
      return 2;
    }
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  const std::vector<std::uint8_t> ts {std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>()};
  if (ts.empty()) {
    fprintf(stderr, "%s is empty\n", argv[1]);
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
  std::thread feeder {[&ring, &ts]() -> void {
    const std::size_t datagrams = (ts.size() + k_datagram - 1) / k_datagram;
    const auto period = std::chrono::duration<double> {
        k_fixture_seconds / static_cast<double>(datagrams)};
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
  const bool failed = dec.failed();
  dec.stop();

  fprintf(stderr, "[decode_file] frames published to the frame ring: %llu "
                  "failed=%d\n",
          static_cast<unsigned long long>(frames), failed ? 1 : 0);
  printf("fixture=%s codec=%s frames=%llu failed=%d\n", argv[1],
         in_codec == codec::av1 ? "av1"
         : in_codec == codec::h264 ? "h264"
                                   : "h265",
         static_cast<unsigned long long>(frames), failed ? 1 : 0);
  return 0;
}
