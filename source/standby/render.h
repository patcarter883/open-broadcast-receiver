// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_RENDER_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_RENDER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace standby
{

// The two elementary streams a standby slate is made of. Both are raw ES in
// the session's ingest codec (video) plus silent AAC-LC (audio), so the shared
// stage can push them into the same selectors and muxers the live media uses
// without a second set of caps.
struct slate_assets
{
  std::string video_path;
  std::string audio_path;
};

enum class slate_codec : std::uint8_t
{
  h264,
  h265,
  av1
};

struct slate_request
{
  std::string codec = "h264";  // the session's in_codec: "h264"|"h265"|"av1"
  int width = 0;               // 0 = unknown; render_slate refuses it
  int height = 0;
  int fps = 0;                 // the slate is 2 seconds long: 2*fps frames
  int bitrate_kbps = 1500;
  std::string logo_path;       // the Open Broadcast mark, bottom/top-left
  std::string brand_path;      // optional per-account image, top-right
  std::string text = "stream standby";
  std::string out_dir;         // cache directory (created if absent)
  // Empty = pick the first present encoder, hardware first. Set to pin one
  // (tests pin the software encoder so a missing VA render node cannot make a
  // logic test fail).
  std::string encoder_override;
};

auto parse_slate_codec(std::string_view name, slate_codec& out) -> bool;
auto slate_codec_name(slate_codec cod) -> const char*;

// Encoder alternatives, hardware first. No silent CPU fallback at the *policy*
// level is not possible here (a slate on the /start path must be able to fall
// back), so the list carries both and the first present one wins.
auto slate_encoder_alternatives(slate_codec cod) -> std::vector<std::string>;

// The ES parser that terminates each access unit: h264parse / h265parse /
// av1parse. config-interval=-1 is applied to h264/h265 (an ES with no repeated
// SPS/PPS cannot be joined by a decoder that attaches mid-loop); av1parse has
// no such property, so it is only named.
auto slate_parser(slate_codec cod) -> const char*;

// File extension of the cached video ES: h264 / h265 / av1.
auto slate_extension(slate_codec cod) -> const char*;

// The caps the parser is pinned to. h264/h265 are byte-stream Annex-B (that is
// what the AU splitter walks and what a mid-loop decoder needs). AV1 is
// "obu-stream" at frame alignment: that is the format mpegtsmux and tsdemux
// carry in this stack, so a slate keyframe is one whole frame per buffer.
auto slate_parse_caps(slate_codec cod) -> const char*;

// The rendered plan, separated from running it so the pipeline strings can be
// asserted without a media stack doing anything: no secrets, no timestamps.
struct slate_plan
{
  std::string key;  // 16 hex chars of sha256(codec|w|h|fps|logo|brand|text)
  std::string video_pipeline;
  std::string audio_pipeline;
  std::string video_path;
  std::string audio_path;
  std::string encoder;      // chosen encoder element, empty when none present
  bool brand_overlay = false;
};

// sha256 of the identity string, truncated to 16 hex chars. A geometry, text
// or branding change is a different slate; anything else reuses the cache.
auto slate_cache_key(const slate_request& req) -> std::string;

// Build the plan. Returns nullopt with err set when the request is unusable
// (unknown codec, non-positive geometry) or no encoder for the codec is
// installed. `present` is the element-presence probe (the registry in
// production, a stub in tests). The brand overlay is included only when
// brand_path is non-empty AND the file exists -- a missing brand image is
// logged by the caller and never fails the render.
using slate_element_present_fn = std::function<bool(const char* element)>;

auto plan_slate(const slate_request& req,
                const slate_element_present_fn& present, std::string& err)
    -> std::optional<slate_plan>;

// Render both ES files for this request, or return the cached pair. Blocking,
// bounded (5 s per pipeline) and non-throwing: it runs on the /start path, so
// a slow or broken encoder must degrade the slate, never the session.
auto render_slate(const slate_request& req, std::string& err)
    -> std::optional<slate_assets>;

}  // namespace standby

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_STANDBY_RENDER_H
