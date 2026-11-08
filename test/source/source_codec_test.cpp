// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// source_codec_test — the comparison between the codec /start declares and the
// media type the ingest's video pad carries.
//
// The regression this pins: an AV1 pad linked into a branch built from a
// declared h264 codec refuses negotiation and starves every transcode output,
// while the copy path keeps reporting healthy. The pad's media type is known
// when tsdemux exposes it, so the mismatch is refused there and the reason
// names both values -- bounded, because it is carried into session state.
//
// Framework-free; exit 0 = pass. No GStreamer.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "fanout/source_codec.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}
}  // namespace

auto main() -> int
{
  using namespace source_codec;

  // The media type a declared codec arrives as.
  expect(media_type(codec::h264) == "video/x-h264", "h264 media type");
  expect(media_type(codec::h265) == "video/x-h265", "h265 media type");
  expect(media_type(codec::av1) == "video/x-av1", "av1 media type");

  // A pad of the declared codec is accepted, whatever else its caps carry.
  expect(accepts(codec::av1, "video/x-av1"), "av1 pad accepted");
  expect(accepts(codec::av1,
                 "video/x-av1 profile=main, chroma-format=4:2:0, "
                 "bit-depth-luma=8, alignment=frame"),
         "a full av1 caps string is accepted");
  expect(mismatch_reason(codec::av1, "video/x-av1 profile=main").empty(),
         "no reason when the pad is the declared codec");

  // A pad of another codec is refused by name: the message carries the declared
  // codec and the caps the pad actually carries.
  {
    const std::string reason = mismatch_reason(
        codec::av1, "video/x-h264 profile=high, stream-format=byte-stream");
    expect(!reason.empty(), "an h264 pad is refused against declared av1");
    expect(reason.find("declared av1") != std::string::npos,
           "the reason names the declared codec");
    expect(reason.find("video/x-h264") != std::string::npos,
           "the reason names the pad's media type");
    expect(!accepts(codec::av1, "video/x-h264 profile=high"),
           "an h264 pad is not the declared av1");
  }
  {
    const std::string reason =
        mismatch_reason(codec::h265, "video/x-h264 profile=main");
    expect(reason.find("declared h265") != std::string::npos
               && reason.find("video/x-h264") != std::string::npos,
           "h265 against an h264 pad names both");
  }
  {
    const std::string reason =
        mismatch_reason(codec::h264, "video/x-av1 profile=main");
    expect(reason.find("declared h264") != std::string::npos
               && reason.find("video/x-av1") != std::string::npos,
           "h264 against an av1 pad names both");
  }

  // The reason is carried into session state, so its length is bounded however
  // long the caps dump that produced it is.
  {
    std::string caps = "video/x-h264 profile=high, level=4.2";
    for (int i = 0; i < 200; ++i) {
      caps += ", field=value";
    }
    const std::string reason = mismatch_reason(codec::av1, caps);
    expect(reason.size() <= k_max_reason_chars,
           "the reason is truncated to k_max_reason_chars");
    expect(reason.find("declared av1") != std::string::npos,
           "truncation keeps the declared codec");
    expect(reason.find("video/x-h264") != std::string::npos,
           "truncation keeps the pad's media type");
  }

  std::printf("source_codec_test: ok\n");
  return 0;
}
