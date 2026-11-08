// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#ifndef OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_SOURCE_CODEC_H
#define OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_SOURCE_CODEC_H

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include "lib/lib.h"

// source_codec — the codec /start declares (`source.codec`) against the media
// type the ingest's video pad actually carries.
//
// The declared codec selects the parser and decoder the transcode branch
// builds, so a stream that arrives as another codec cannot traverse that
// branch: the parser refuses the caps, nothing reaches the decoder, and every
// transcode output starves while the copy path keeps reporting healthy. The
// pad's media type is known the moment tsdemux exposes the pad, so the two are
// compared there and a mismatch is refused with both values named.
namespace source_codec
{

// Cap on the reason carried into session state: it is rendered by the panel and
// sent over the control plane, so it never grows with the caps dump that
// produced it.
inline constexpr std::size_t k_max_reason_chars = 160;

// Media type a declared source codec arrives as.
[[nodiscard]] inline auto media_type(codec declared) noexcept -> std::string_view
{
  switch (declared)
  {
    case codec::h264:
      return "video/x-h264";
    case codec::h265:
      return "video/x-h265";
    case codec::av1:
      return "video/x-av1";
  }
  return {};
}

// The media type part of a caps string ("video/x-h264 profile=high ...").
[[nodiscard]] inline auto caps_media_type(std::string_view caps) noexcept
    -> std::string_view
{
  const auto end = caps.find_first_of(", ");
  return caps.substr(0, end == std::string_view::npos ? caps.size() : end);
}

// True when a pad carrying `pad_caps` is the codec /start declared.
[[nodiscard]] inline auto accepts(codec declared,
                                  std::string_view pad_caps) noexcept -> bool
{
  return caps_media_type(pad_caps) == media_type(declared);
}

// The reason a mismatched video pad is refused, naming the declared codec and
// the caps the pad carries, truncated to k_max_reason_chars. Empty when the pad
// matches the declared codec.
[[nodiscard]] inline auto mismatch_reason(codec declared,
                                          std::string_view actual_caps)
    -> std::string
{
  if (accepts(declared, actual_caps))
  {
    return {};
  }
  std::string reason = std::format(
      "source codec mismatch: declared {} but the ingest video pad carries {}",
      to_string(declared), actual_caps);
  if (reason.size() > k_max_reason_chars)
  {
    reason.resize(k_max_reason_chars);
  }
  return reason;
}

}  // namespace source_codec

#endif  // OPEN_BROADCAST_RECEIVER_SOURCE_FANOUT_SOURCE_CODEC_H
