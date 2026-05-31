#include "lib/lib.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// ---------------------------------------------------------------------------
// Internal helpers and validation constants (translation-unit scope)
// ---------------------------------------------------------------------------

namespace
{
// Validation limits (CONTRACT §4)
constexpr int k_port_min = 1;
constexpr int k_port_max = 65535;
constexpr unsigned char k_first_printable = 0x20;
constexpr unsigned char k_del = 0x7f;
constexpr int k_bandwidth_min = 100;
constexpr int k_bandwidth_max = 100000;
constexpr int k_buffer_min_max = 30000;
constexpr int k_buffer_max_max = 60000;
constexpr int k_rtt_min_max = 10000;
constexpr int k_rtt_max_max = 60000;
constexpr int k_reorder_max = 10000;
constexpr int k_video_bitrate_min = 1000;
constexpr int k_video_bitrate_max = 60000;
constexpr int k_dim_min = 16;
constexpr int k_dim_max_width = 7680;
constexpr int k_dim_max_height = 4320;
constexpr int k_audio_bitrate_min = 32;
constexpr int k_audio_bitrate_max = 512;
constexpr int k_latency_max = 8000;
constexpr int k_sender_buffer_max = 10000;

auto expected_scheme(output_proto proto) noexcept -> const char*
{
  switch (proto) {
    case output_proto::rtmp:
      return "rtmp://";
    case output_proto::rtmps:
      return "rtmps://";
    case output_proto::srt:
      return "srt://";
    case output_proto::rist:
      return "rist://";
  }
  return "";
}

auto valid_host(std::string_view host) noexcept -> bool
{
  if (host.empty()) {
    return false;
  }
  return std::ranges::all_of(host, [](char chr) noexcept -> bool {
    const auto byte = static_cast<unsigned char>(chr);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
        || (byte >= '0' && byte <= '9') || chr == '.' || chr == '-' || chr == '_'
        || chr == ':' || chr == '[' || chr == ']';
  });
}

auto fail(std::string code,
          std::string field,
          std::string msg) -> validation_result
{
  return {
    .ok = false,
    .error_code = std::move(code),
    .field = std::move(field),
    .message = std::move(msg)
  };
}

auto validate_ingest(const ingest_config& ing) -> validation_result
{
  if (ing.bandwidth < k_bandwidth_min || ing.bandwidth > k_bandwidth_max) {
    return fail("out_of_range", "ingest.bandwidth", "must be 100..100000");
  }
  if (ing.buffer_min < 0 || ing.buffer_min > k_buffer_min_max) {
    return fail("out_of_range", "ingest.buffer_min", "must be 0..30000");
  }
  if (ing.buffer_max < ing.buffer_min || ing.buffer_max > k_buffer_max_max) {
    return fail(
        "out_of_range", "ingest.buffer_max", "must be buffer_min..60000");
  }
  if (ing.rtt_min < 0 || ing.rtt_min > k_rtt_min_max) {
    return fail("out_of_range", "ingest.rtt_min", "must be 0..10000");
  }
  if (ing.rtt_max < ing.rtt_min || ing.rtt_max > k_rtt_max_max) {
    return fail("out_of_range", "ingest.rtt_max", "must be rtt_min..60000");
  }
  if (ing.reorder_buffer < 0 || ing.reorder_buffer > k_reorder_max) {
    return fail("out_of_range", "ingest.reorder_buffer", "must be 0..10000");
  }
  if (!ing.rist_listen.starts_with("rist://@")
      && !ing.rist_listen.starts_with("rist6://@"))
  {
    return fail("bad_url",
                "ingest.rist_listen",
                "must be a rist listener URL beginning rist://@");
  }
  return {};
}

auto validate_srt_rist_host(const destination& dst,
                              const std::string& pfx) -> validation_result
{
  std::string host;
  int port = 0;
  if (!parse_authority(dst.url, host, port)) {
    return fail("bad_url", pfx + ".url", "could not parse host:port from url");
  }
  if (!valid_host(host)) {
    return fail("bad_url", pfx + ".url", "url host contains invalid characters");
  }
  if (dst.proto == output_proto::rist && (port % 2) != 0) {
    return fail("out_of_range",
                pfx + ".url",
                "rist port must be even (RTCP uses port+1)");
  }
  // ristsink cname is interpolated UNQUOTED, so a non-empty cname must be a
  // clean token too.
  if (dst.proto == output_proto::rist && !dst.cname.empty()
      && !valid_host(dst.cname))
  {
    return fail("bad_url", pfx + ".params.cname", "cname must be a token");
  }
  return {};
}

auto validate_video_reencode(const destination& dst,
                              const std::string& pfx) -> validation_result
{
  if (dst.video.bitrate_kbps < k_video_bitrate_min
      || dst.video.bitrate_kbps > k_video_bitrate_max)
  {
    return fail("out_of_range",
                pfx + ".video.bitrate_kbps",
                "must be 1000..60000");
  }
  if (!dst.video.upscale) {
    return {};
  }
  if (dst.video.width < k_dim_min || dst.video.width > k_dim_max_width
      || (dst.video.width % 2) != 0)
  {
    return fail("out_of_range", pfx + ".video.width", "must be even, 16..7680");
  }
  if (dst.video.height < k_dim_min || dst.video.height > k_dim_max_height
      || (dst.video.height % 2) != 0)
  {
    return fail("out_of_range",
                pfx + ".video.height",
                "must be even, 16..4320");
  }
  return {};
}

auto validate_destination(const destination& dst,
                           std::size_t idx,
                           codec in_codec) -> validation_result
{
  const std::string pfx = std::format("outputs[{}]", idx);

  if (dst.id.empty()) {
    return fail("invalid_schema", pfx + ".id", "output id is required");
  }

  if (!dst.url.starts_with(expected_scheme(dst.proto))) {
    return fail("bad_url",
                pfx + ".url",
                std::format("url scheme must match type '{}'",
                            to_string(dst.proto)));
  }
  if (!is_pipeline_safe(dst.url)) {
    return fail("bad_url", pfx + ".url", "url contains forbidden characters");
  }
  if (!is_pipeline_safe(dst.key_or_streamid)) {
    return fail("bad_url",
                pfx + ".key_or_streamid",
                "key_or_streamid contains forbidden characters");
  }
  if (!is_pipeline_safe(dst.cname)) {
    return fail("bad_url", pfx + ".cname", "cname contains forbidden characters");
  }

  if (dst.proto == output_proto::srt || dst.proto == output_proto::rist) {
    if (auto res = validate_srt_rist_host(dst, pfx); !res.ok) {
      return res;
    }
  }

  const codec eff_codec = dst.video.reencode ? dst.video.out_codec : in_codec;

  if (!dst.video.reencode && dst.video.out_codec != in_codec) {
    return fail("bad_enum",
                pfx + ".video.codec",
                "copy mode requires video.codec == source.codec");
  }

  // RTMP/flvmux carries only H.264 (+AAC). See docs/GSTREAMER.md §9.
  if ((dst.proto == output_proto::rtmp || dst.proto == output_proto::rtmps)
      && eff_codec != codec::h264)
  {
    return fail(
        "rtmp_codec_unsupported",
        pfx + ".video.codec",
        "RTMP/flvmux can only carry H.264; choose h264 (reencode) or a "
        "non-RTMP output for this codec");
  }

  if (dst.video.reencode) {
    if (auto res = validate_video_reencode(dst, pfx); !res.ok) {
      return res;
    }
  }

  if (dst.audio.reencode
      && (dst.audio.bitrate_kbps < k_audio_bitrate_min
          || dst.audio.bitrate_kbps > k_audio_bitrate_max))
  {
    return fail(
        "out_of_range", pfx + ".audio.bitrate_kbps", "must be 32..512");
  }

  if (dst.proto == output_proto::srt
      && (dst.latency_ms < 0 || dst.latency_ms > k_latency_max))
  {
    return fail("out_of_range",
                pfx + ".params.latency_ms",
                "must be 0..8000");
  }
  if (dst.proto == output_proto::rist
      && (dst.sender_buffer < 0 || dst.sender_buffer > k_sender_buffer_max))
  {
    return fail(
        "out_of_range", pfx + ".params.sender_buffer", "must be 0..10000");
  }

  return {};
}
}  // namespace

// ---------------------------------------------------------------------------
// Enum <-> string (public API)
// ---------------------------------------------------------------------------

auto to_string(codec cod) noexcept -> const char*
{
  switch (cod) {
    case codec::h264:
      return "h264";
    case codec::h265:
      return "h265";
    case codec::av1:
      return "av1";
  }
  return "h264";
}

auto to_string(encoder enc) noexcept -> const char*
{
  switch (enc) {
    case encoder::amd:
      return "amd";
    case encoder::qsv:
      return "qsv";
    case encoder::nvenc:
      return "nvenc";
    case encoder::software:
      return "software";
  }
  return "software";
}

auto to_string(output_proto proto) noexcept -> const char*
{
  switch (proto) {
    case output_proto::rtmp:
      return "rtmp";
    case output_proto::rtmps:
      return "rtmps";
    case output_proto::srt:
      return "srt";
    case output_proto::rist:
      return "rist";
  }
  return "rtmp";
}

auto parse_codec(std::string_view str, codec& out) noexcept -> bool
{
  if (str == "h264") {
    out = codec::h264;
  } else if (str == "h265") {
    out = codec::h265;
  } else if (str == "av1") {
    out = codec::av1;
  } else {
    return false;
  }
  return true;
}

auto parse_encoder(std::string_view str, encoder& out) noexcept -> bool
{
  if (str == "amd") {
    out = encoder::amd;
  } else if (str == "qsv") {
    out = encoder::qsv;
  } else if (str == "nvenc") {
    out = encoder::nvenc;
  } else if (str == "software") {
    out = encoder::software;
  } else {
    return false;
  }
  return true;
}

auto parse_output_proto(std::string_view str, output_proto& out) noexcept -> bool
{
  if (str == "rtmp") {
    out = output_proto::rtmp;
  } else if (str == "rtmps") {
    out = output_proto::rtmps;
  } else if (str == "srt") {
    out = output_proto::srt;
  } else if (str == "rist") {
    out = output_proto::rist;
  } else {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// URL / host utilities (public API)
// ---------------------------------------------------------------------------

auto listen_port_from_url(const std::string& rist_listen) noexcept -> int
{
  // Strip any query string.
  const std::string base = rist_listen.substr(0, rist_listen.find('?'));
  // IPv6 bracketed form: port follows the "]:".
  std::size_t colon = std::string::npos;
  const std::size_t bracket = base.rfind(']');
  if (bracket != std::string::npos) {
    colon = base.find(':', bracket);
  } else {
    colon = base.rfind(':');
  }
  if (colon == std::string::npos || colon + 1 >= base.size()) {
    return -1;
  }
  try {
    const int port = std::stoi(base.substr(colon + 1));
    if (port < k_port_min || port > k_port_max) {
      return -1;
    }
    return port;
  } catch (...) {
    return -1;
  }
}

auto parse_authority(const std::string& url,
                     std::string& host,
                     int& port) noexcept -> bool
{
  const std::size_t scheme = url.find("://");
  if (scheme == std::string::npos) {
    return false;
  }
  std::string rest = url.substr(scheme + 3);
  const std::size_t cut = rest.find_first_of("/?");
  if (cut != std::string::npos) {
    rest = rest.substr(0, cut);
  }
  std::size_t colon = std::string::npos;
  const std::size_t bracket = rest.rfind(']');
  if (bracket != std::string::npos) {  // [IPv6]:port
    host = rest.substr(0, bracket + 1);
    colon = rest.find(':', bracket);
  } else {
    colon = rest.rfind(':');
    if (colon != std::string::npos) {
      host = rest.substr(0, colon);
    }
  }
  if (colon == std::string::npos || colon + 1 >= rest.size()) {
    return false;
  }
  try {
    port = std::stoi(rest.substr(colon + 1));
  } catch (...) {
    return false;
  }
  return port >= k_port_min && port <= k_port_max;
}

auto build_listener_url(const ingest_config& ingest) -> std::string
{
  // Base = everything up to the first '?' of the configured listen URL
  // (scheme + "@host:port"). Defaults to rist://@[::]:5000.
  std::string base = ingest.rist_listen.substr(0, ingest.rist_listen.find('?'));
  if (base.empty()) {
    base = "rist://@[::]:5000";
  }
  // NOTE: do NOT append a `profile=` URL parameter — librist's URL parser
  // rejects it ("Unknown or invalid parameter profile") and fails the whole
  // listener. The profile is set via RISTNetReceiverSettings.mProfile instead.
  return std::format(
      "{}?bandwidth={}&buffer-min={}&buffer-max={}&rtt-min={}&rtt-max={}&"
      "reorder-buffer={}&timing-mode=2",
      base,
      ingest.bandwidth,
      ingest.buffer_min,
      ingest.buffer_max,
      ingest.rtt_min,
      ingest.rtt_max,
      ingest.reorder_buffer);
}

auto is_pipeline_safe(std::string_view str) noexcept -> bool
{
  return std::ranges::all_of(str, [](char chr) noexcept -> bool {
    const auto byte = static_cast<unsigned char>(chr);
    return byte >= k_first_printable && byte != k_del
        && chr != '\'' && chr != '"' && chr != '\\';
  });
}

// ---------------------------------------------------------------------------
// Validation entry point (public API)
// ---------------------------------------------------------------------------

auto validate_config(const receiver_config& cfg) -> validation_result
{
  if (cfg.schema_version != 1) {
    return fail("invalid_schema",
                "schema_version",
                "unsupported schema_version (expected 1)");
  }

  if (const auto res = validate_ingest(cfg.ingest); !res.ok) {
    return res;
  }

  if (cfg.destinations.empty()) {
    return fail("invalid_schema", "outputs", "at least one output is required");
  }

  for (std::size_t idx = 0; idx < cfg.destinations.size(); ++idx) {
    if (auto res = validate_destination(cfg.destinations.at(idx), idx, cfg.in_codec);
        !res.ok)
    {
      return res;
    }
  }

  return {};
}
