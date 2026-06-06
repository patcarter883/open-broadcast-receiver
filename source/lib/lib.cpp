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
constexpr int k_gop_min = 1;
constexpr int k_gop_max = 600;

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

// A bare token: ASCII alphanumerics, '_' and '-' only (pixel-format names like
// NV12 / I420 / YUY2). Used to keep CLI-supplied values that are interpolated
// UNQUOTED-ish into caps strings free of any pipeline-meta characters.
auto is_token(std::string_view str) noexcept -> bool
{
  if (str.empty()) {
    return false;
  }
  return std::ranges::all_of(str, [](char chr) noexcept -> bool {
    const auto byte = static_cast<unsigned char>(chr);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
        || (byte >= '0' && byte <= '9') || chr == '_' || chr == '-';
  });
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

// The on-GPU re-encode settings come from the encoder (bitrate/upscale/dims) and
// the operator (udp_host/udp_port/preset). The host and preset go bare into the
// udpsink/nvh264enc pipeline string, so both must be free of whitespace /
// quote-escape / control characters. The output is H264 only (stream2 has no
// nvh265enc / nvav1enc), so out_codec is pinned to h264.
auto validate_reencode(const reencode_config& re) -> validation_result
{
  if (re.udp_host.empty() || !is_pipeline_safe(re.udp_host)) {
    return fail("bad_host",
                "reencode.udp_host",
                "must be a non-empty pipeline-safe host/IP");
  }
  if (re.udp_port < 1 || re.udp_port > 65535) {
    return fail("out_of_range", "reencode.udp_port", "must be 1..65535");
  }
  if (re.bitrate_kbps < k_video_bitrate_min
      || re.bitrate_kbps > k_video_bitrate_max)
  {
    return fail(
        "out_of_range", "reencode.bitrate_kbps", "must be 1000..60000");
  }
  if (re.gop_size < k_gop_min || re.gop_size > k_gop_max) {
    return fail("out_of_range", "reencode.gop_size", "must be 1..600");
  }
  if (!is_token(re.preset)) {
    return fail("bad_enum",
                "reencode.preset",
                "preset must be a bare token (e.g. low-latency-hq)");
  }
  if (re.upscale) {
    if (re.width % 2 != 0 || re.height % 2 != 0 || re.width < k_dim_min
        || re.width > k_dim_max_width || re.height < k_dim_min
        || re.height > k_dim_max_height)
    {
      return fail("out_of_range",
                  "reencode.width",
                  "upscale dims must be even, width 16..7680 height 16..4320");
    }
  }
  if (re.out_codec != codec::h264) {
    return fail("bad_enum",
                "reencode.out_codec",
                "rtmp output requires h264");
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

auto to_string(audio_codec cod) noexcept -> const char*
{
  switch (cod) {
    case audio_codec::aac:
      return "aac";
    case audio_codec::opus:
      return "opus";
    case audio_codec::ac3:
      return "ac3";
    case audio_codec::eac3:
      return "eac3";
    case audio_codec::mp2:
      return "mp2";
  }
  return "aac";
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
      // timing-mode=0 (SOURCE) — the librist default. NOT 1 (ARRIVAL), NOT 2
      // (RTC). RTC drops every packet until an RTCP SR sets time_offset (the
      // sender provides none, ts_ntp=0). ARRIVAL interpolates the arrival time
      // of *retransmitted* packets and asserts packet_time < next->packet_time
      // (rist-common.c); the extra retries of the encoder->rist2rist->receiver
      // double hop violate that invariant and SIGABRT this receiver. SOURCE
      // orders/paces by the monotonic source timestamp librist stamps on each
      // packet (preserved across the relay) and never enters that path. Must
      // match the encoder and rist2rist.
      "{}?bandwidth={}&buffer-min={}&buffer-max={}&rtt-min={}&rtt-max={}&"
      "reorder-buffer={}&timing-mode=0",
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

  if (const auto res = validate_reencode(cfg.reencode); !res.ok) {
    return res;
  }

  return {};
}
