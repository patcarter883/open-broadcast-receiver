// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// es_looper_test — the access-unit splitter that decides where the standby
// slate may be spliced (plan B2).
//
// This is the cheapest possible guard against the most expensive failure the
// slate can have: a splice inside an access unit decodes as a torn picture on
// every destination, and nothing downstream (the re-stamper, the muxer) can
// tell that apart from a corrupt encoder feed. The splitter is pure, so the
// boundaries are pinned here without a media stack.
//
// Covers both framings the renderer can produce: Annex-B (h264/h265) and AV1's
// obu-stream (temporal-delimiter delimited). Framework-free; exit 0 = pass.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "standby/es_looper.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto expect_span(const standby::au_span& span, std::size_t offset,
                 std::size_t length, const char* what) -> void
{
  if (span.first != offset || span.second != length) {
    std::fprintf(stderr, "FAIL: %s (got [%zu,%zu), expected [%zu,%zu))\n", what,
                 span.first, span.second, offset, length);
    std::exit(1);
  }
}

auto bytes(std::initializer_list<int> values) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  out.reserve(values.size());
  for (const int v : values) {
    out.push_back(static_cast<std::uint8_t>(v));
  }
  return out;
}
}  // namespace

auto main() -> int
{
  using standby::au_is_keyframe;
  using standby::split_access_units;

  // Four AUs, mixing 3- and 4-byte start codes. Every span must start on its
  // own start code, and the spans must tile the file exactly: no gap, no
  // overlap, no partial AU at either end.
  {
    const auto data = bytes({
        0x00, 0x00, 0x00, 0x01, 0x65, 0xaa, 0xbb,  //  [0,7)   IDR, 4-byte code
        0x00, 0x00, 0x01, 0x41, 0xcc,              //  [7,12)  P,   3-byte code
        0x00, 0x00, 0x00, 0x01, 0x41, 0xdd, 0xee,  //  [12,19) P,   4-byte code
        0x00, 0x00, 0x01, 0x41, 0xff,              //  [19,24) P,   3-byte code
    });
    const auto aus = split_access_units(data.data(), data.size());
    expect(aus.size() == 4, "four start codes produce four AUs");
    expect_span(aus[0], 0, 7, "AU 1 keeps its 4-byte start code");
    expect_span(aus[1], 7, 5, "AU 2 starts on its 3-byte start code");
    expect_span(aus[2], 12, 7, "AU 3 keeps its 4-byte start code");
    expect_span(aus[3], 19, 5, "AU 4 runs to end of file");
    expect(aus.front().first == 0 && aus.back().first + aus.back().second
                                       == data.size(),
           "the spans tile the file with no partial AU");

    // This is the AU the loop restarts on: it must read as a keyframe.
    expect(au_is_keyframe(standby::es_codec::h264, data.data() + aus[0].first,
                          aus[0].second),
           "the first AU is an IDR");
    expect(!au_is_keyframe(standby::es_codec::h264, data.data() + aus[1].first,
                           aus[1].second),
           "a non-IDR slice is not a keyframe");
    // The same byte read as h265 is a VPS (type 32) -- which is exactly why the
    // codec can never be guessed from the NAL header alone.
    expect(au_is_keyframe(standby::es_codec::h265, data.data() + aus[1].first,
                          aus[1].second),
           "0x41 is a parameter set when read as h265");
  }

  // A 4-byte start code is ONE boundary, not a 3-byte boundary plus a stray
  // leading zero AU. The reported offset is the same for either reading, and
  // the previous AU is not extended into the next.
  {
    const auto data = bytes({0x00, 0x00, 0x01, 0x65, 0x11,
                             0x00, 0x00, 0x00, 0x01, 0x41, 0x22});
    const auto aus = split_access_units(data.data(), data.size());
    expect(aus.size() == 2, "00 00 00 01 does not split into three");
    expect_span(aus[0], 0, 5, "first AU ends at the 4-byte code");
    expect_span(aus[1], 5, 6, "second AU is the whole 4-byte-coded AU");
  }

  // A stream that carries access-unit delimiters is grouped by them: the unit
  // is AUD + parameter sets + one picture, not one NAL. This is what every
  // encoder the renderer can pick actually writes, so a per-NAL split here
  // would quadruple the frame rate of the slate on air.
  {
    const auto data = bytes({
        0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,              // AUD
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,              // SPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce,              // PPS
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84,        // IDR slice
        0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,              // AUD
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22,        // P slice
        0x00, 0x00, 0x01, 0x09, 0xf0,                    // AUD (3-byte code)
        0x00, 0x00, 0x01, 0x41, 0x9a, 0x33,              // P slice
    });
    const auto aus = split_access_units(data.data(), data.size());
    expect(aus.size() == 3, "an AUD-delimited stream has one AU per AUD");
    expect_span(aus[0], 0, 25, "AU 1 is AUD + SPS + PPS + IDR");
    expect_span(aus[1], 25, 13, "AU 2 starts at its AUD");
    expect_span(aus[2], 38, 11, "AU 3 uses a 3-byte AUD start code");
    expect(au_is_keyframe(standby::es_codec::h264, data.data() + aus[0].first,
                          aus[0].second),
           "the unit is a keyframe even though it opens with an AUD");
    expect(!au_is_keyframe(standby::es_codec::h264, data.data() + aus[1].first,
                           aus[1].second),
           "a unit whose only picture is a P slice is not a keyframe");
  }

  // Bytes before the first start code are not an access unit (a file that
  // begins with a partial fragment must not be replayed from that fragment).
  {
    const auto data = bytes({0xde, 0xad, 0xbe, 0xef,
                             0x00, 0x00, 0x01, 0x65, 0x11,
                             0x00, 0x00, 0x01, 0x41, 0x22});
    const auto aus = split_access_units(data.data(), data.size());
    expect(aus.size() == 2, "leading junk is skipped");
    expect_span(aus[0], 4, 5, "the first AU starts at the first start code");
  }

  // Degenerate inputs: no start code at all, empty, null. No exception, no
  // bogus AU -- the looper treats an empty split as "no slate".
  {
    const auto none = bytes({0x65, 0x01, 0x02, 0x03});
    expect(split_access_units(none.data(), none.size()).empty(),
           "a stream with no start code has no AUs");
    expect(split_access_units(nullptr, 16).empty(), "null data -> no AUs");
    const auto tiny = bytes({0x00, 0x00});
    expect(split_access_units(tiny.data(), tiny.size()).empty(),
           "a 2-byte buffer has no AUs");
  }

  // h265: a VPS/SPS-prefixed AU opens the stream, then an IDR (type 19).
  {
    const auto vps = bytes({0x00, 0x00, 0x00, 0x01, 0x40, 0x01});
    const auto idr = bytes({0x00, 0x00, 0x00, 0x01, 0x26, 0x01});
    const auto trail = bytes({0x00, 0x00, 0x00, 0x01, 0x02, 0x01});
    expect(au_is_keyframe(standby::es_codec::h265, vps.data(), vps.size()),
           "h265 VPS starts decoding");
    expect(au_is_keyframe(standby::es_codec::h265, idr.data(), idr.size()),
           "h265 IDR (type 19)");
    expect(!au_is_keyframe(standby::es_codec::h265, trail.data(), trail.size()),
           "h265 TRAIL_R is not a keyframe");
    expect(!au_is_keyframe(standby::es_codec::h264, vps.data(), vps.size()),
           "0x40 is not an h264 keyframe");
    // An access unit delimiter is not an entry point in either codec.
    const auto aud = bytes({0x00, 0x00, 0x01, 0x09, 0x10});
    expect(!au_is_keyframe(standby::es_codec::h264, aud.data(), aud.size()),
           "h264 AUD is not a keyframe");
  }

  // AV1 obu-stream: a temporal delimiter (OBU type 2) opens each AU, and the
  // split walks the OBU framing rather than searching for a byte pattern, so a
  // payload containing 00 00 01 cannot split an AU.
  {
    const auto td = bytes({0x12, 0x00});                     // TD, no payload
    const auto seq = bytes({0x0a, 0x02, 0x00, 0x00});        // sequence header
    const auto key = bytes({0x1a, 0x01, 0x10});              // frame header?
    const auto inter = bytes({0x1a, 0x01, 0x40});

    std::vector<std::uint8_t> data;
    const auto append = [&data](const std::vector<std::uint8_t>& part) {
      data.insert(data.end(), part.begin(), part.end());
    };
    append(td);
    append(seq);
    append(key);
    append(td);
    append(inter);

    const auto aus = standby::split_obu_access_units(data.data(), data.size());
    expect(aus.size() == 2, "two temporal delimiters = two AUs");
    expect_span(aus[0], 0, 9, "first AU is the key frame with its delimiter");
    expect_span(aus[1], 9, 5, "second AU starts at its delimiter");
  }

  // The OBU reader must not be fooled by an Annex-B-looking payload.
  {
    // TD (0x12 0x00), then a frame OBU whose payload contains 00 00 01.
    const auto au = bytes({0x12, 0x00, 0x1a, 0x04, 0x00, 0x00, 0x01, 0x00});
    const auto aus = standby::split_obu_access_units(au.data(), au.size());
    expect(aus.size() == 1, "a start code inside an OBU payload is not a splice");
    expect_span(aus[0], 0, au.size(), "the whole buffer is one AU");
  }

  // AV1 keyframe detection reads the frame header's frame_type.
  {
    // TD + FRAME_HEADER (type 3, size 1) with show_existing=0, frame_type=0.
    const auto keyframe = bytes({0x12, 0x00, 0x1a, 0x01, 0x10});
    expect(standby::obu_au_is_keyframe(keyframe.data(), keyframe.size()),
           "AV1 KEY_FRAME is a keyframe");
    const auto inter = bytes({0x12, 0x00, 0x1a, 0x01, 0x40});
    expect(!standby::obu_au_is_keyframe(inter.data(), inter.size()),
           "AV1 inter frame is not a keyframe");
  }

  std::puts("es_looper_test: ALL OK");
  return 0;
}
