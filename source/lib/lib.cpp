// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include "lib/lib.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

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
constexpr int k_default_rtmp_port = 1935;
constexpr int k_default_rtmps_port = 443;

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

auto scheme_of(const std::string& url) -> std::string
{
  const std::size_t pos = url.find("://");
  if (pos == std::string::npos) {
    return {};
  }
  std::string scheme = url.substr(0, pos);
  std::ranges::transform(scheme,
                         scheme.begin(),
                         [](unsigned char chr) -> char
                         { return static_cast<char>(std::tolower(chr)); });
  return scheme;
}

auto expected_scheme(output_proto proto) -> const char*
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
  return "";
}

auto default_port(output_proto proto) -> int
{
  switch (proto) {
    case output_proto::rtmp:
      return k_default_rtmp_port;
    case output_proto::rtmps:
      return k_default_rtmps_port;
    case output_proto::srt:
    case output_proto::rist:
      return 0;  // must be explicit
  }
  return 0;
}

// Parse scheme://host[:port][/path][?query] into host/port/path. Unlike
// parse_authority, the port may be absent (scheme default applied by caller).
auto split_url(const std::string& url,
               std::string& host,
               int& port,
               std::string& path) -> bool
{
  const std::size_t scheme = url.find("://");
  if (scheme == std::string::npos) {
    return false;
  }
  std::string rest = url.substr(scheme + 3);
  const std::size_t cut = rest.find_first_of("/?");
  if (cut != std::string::npos) {
    path = (rest[cut] == '/') ? rest.substr(cut) : std::string {};
    const std::size_t query = path.find('?');
    if (query != std::string::npos) {
      path = path.substr(0, query);
    }
    rest = rest.substr(0, cut);
  } else {
    path.clear();
  }

  port = 0;
  std::size_t colon = std::string::npos;
  const std::size_t bracket = rest.rfind(']');
  if (bracket != std::string::npos) {  // [IPv6](:port)?
    host = rest.substr(0, bracket + 1);
    colon = rest.find(':', bracket);
  } else {
    colon = rest.rfind(':');
    host = (colon != std::string::npos) ? rest.substr(0, colon) : rest;
  }
  if (colon != std::string::npos) {
    if (colon + 1 >= rest.size()) {
      return false;  // trailing colon, no port
    }
    try {
      port = std::stoi(rest.substr(colon + 1));
    } catch (...) {
      return false;
    }
    if (port < k_port_min || port > k_port_max) {
      return false;
    }
  }
  return !host.empty();
}

// Strip the brackets of a bracketed IPv6 literal for inet_pton/getaddrinfo.
auto unbracket(const std::string& host) -> std::string
{
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    return host.substr(1, host.size() - 2);
  }
  return host;
}

// ---------------------------------------------------------------------------
// IP classification (M1.2)
// ---------------------------------------------------------------------------

struct parsed_ip
{
  bool valid = false;
  bool is_v6 = false;
  std::array<uint8_t, 16> bytes {};  // v4 in bytes[0..3]
};

auto parse_ip(const std::string& literal) -> parsed_ip
{
  parsed_ip out;
  in_addr addr4 {};
  if (inet_pton(AF_INET, literal.c_str(), &addr4) == 1) {
    out.valid = true;
    out.is_v6 = false;
    std::memcpy(out.bytes.data(), &addr4, sizeof(addr4));
    return out;
  }
  in6_addr addr6 {};
  if (inet_pton(AF_INET6, unbracket(literal).c_str(), &addr6) == 1) {
    // IPv4-mapped (::ffff:a.b.c.d) classifies as the embedded IPv4 address —
    // otherwise a mapped literal smuggles a forbidden v4 target past the rules.
    if (IN6_IS_ADDR_V4MAPPED(&addr6)) {
      out.valid = true;
      out.is_v6 = false;
      std::memcpy(out.bytes.data(), &addr6.s6_addr[12], 4);
      return out;
    }
    out.valid = true;
    out.is_v6 = true;
    std::memcpy(out.bytes.data(), &addr6, sizeof(addr6));
    return out;
  }
  return out;
}

auto v4_in(const parsed_ip& ip_addr, uint32_t net, int prefix) -> bool
{
  uint32_t host_order = 0;
  std::memcpy(&host_order, ip_addr.bytes.data(), 4);
  host_order = ntohl(host_order);
  const uint32_t mask =
      prefix == 0 ? 0 : (0xFFFFFFFFUL << (32 - static_cast<unsigned>(prefix)));
  return (host_order & mask) == (net & mask);
}

auto v6_prefix(const parsed_ip& ip_addr, uint8_t byte0, uint8_t mask0) -> bool
{
  return (ip_addr.bytes[0] & mask0) == byte0;
}

// nullptr = acceptable; else a short class reason.
auto classify(const parsed_ip& ip_addr, bool allow_private) -> const char*
{
  if (!ip_addr.is_v6) {
    if (v4_in(ip_addr, 0x7F000000, 8)) {  // 127.0.0.0/8
      return "loopback";
    }
    if (v4_in(ip_addr, 0xA9FE0000, 16)) {  // 169.254.0.0/16 (incl. metadata)
      return "link_local";
    }
    if (v4_in(ip_addr, 0xE0000000, 4)) {  // 224.0.0.0/4
      return "multicast";
    }
    if (v4_in(ip_addr, 0x00000000, 8)) {  // 0.0.0.0/8
      return "unspecified";
    }
    if (v4_in(ip_addr, 0xFFFFFFFF, 32)) {  // broadcast
      return "broadcast";
    }
    const bool priv = v4_in(ip_addr, 0x0A000000, 8)  // 10/8
        || v4_in(ip_addr, 0xAC100000, 12)            // 172.16/12
        || v4_in(ip_addr, 0xC0A80000, 16)            // 192.168/16
        || v4_in(ip_addr, 0x64400000, 10);           // 100.64/10 CGNAT
    if (priv && !allow_private) {
      return "private";
    }
    return nullptr;
  }

  static const std::array<uint8_t, 16> k_zero {};
  if (std::memcmp(ip_addr.bytes.data(), k_zero.data(), 16) == 0) {
    return "unspecified";  // ::
  }
  std::array<uint8_t, 16> loop {};
  loop[15] = 1;
  if (std::memcmp(ip_addr.bytes.data(), loop.data(), 16) == 0) {
    return "loopback";  // ::1
  }
  if (v6_prefix(ip_addr, 0xFE, 0xFF)
      && (ip_addr.bytes[1] & 0xC0) == 0x80)  // fe80::/10
  {
    return "link_local";
  }
  if (v6_prefix(ip_addr, 0xFF, 0xFF)) {  // ff00::/8
    return "multicast";
  }
  if ((ip_addr.bytes[0] & 0xFE) == 0xFC && !allow_private) {  // fc00::/7 ULA
    return "private";
  }
  return nullptr;
}

struct cidr
{
  parsed_ip base;
  int prefix = 0;
};

auto parse_cidr(const std::string& text, cidr& out) -> bool
{
  const std::size_t slash = text.find('/');
  const std::string ip_part =
      (slash == std::string::npos) ? text : text.substr(0, slash);
  out.base = parse_ip(ip_part);
  if (!out.base.valid) {
    return false;
  }
  const int full = out.base.is_v6 ? 128 : 32;
  if (slash == std::string::npos) {
    out.prefix = full;
    return true;
  }
  try {
    out.prefix = std::stoi(text.substr(slash + 1));
  } catch (...) {
    return false;
  }
  return out.prefix >= 0 && out.prefix <= full;
}

auto cidr_contains(const cidr& net, const parsed_ip& ip_addr) -> bool
{
  if (net.base.is_v6 != ip_addr.is_v6) {
    return false;
  }
  const int total_bytes = net.base.is_v6 ? 16 : 4;
  int bits = net.prefix;
  for (int idx = 0; idx < total_bytes && bits > 0; ++idx) {
    const int take = std::min(bits, 8);
    const auto mask = static_cast<uint8_t>(0xFF << (8 - take));
    if ((net.base.bytes[static_cast<std::size_t>(idx)] & mask)
        != (ip_addr.bytes[static_cast<std::size_t>(idx)] & mask))
    {
      return false;
    }
    bits -= take;
  }
  return true;
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

auto to_string(transcode_target target) noexcept -> const char*
{
  switch (target) {
    case transcode_target::none:
      return "none";
    case transcode_target::h264:
      return "h264";
    case transcode_target::h265:
      return "h265";
  }
  return "none";
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

auto parse_output_proto(std::string_view str, output_proto& out) noexcept
    -> bool
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

auto parse_transcode_target(std::string_view str,
                            transcode_target& out) noexcept -> bool
{
  if (str == "h264") {
    out = transcode_target::h264;
  } else if (str == "h265") {
    out = transcode_target::h265;
  } else if (str == "av1") {
    out = transcode_target::av1;
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
// Element availability (public API)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Transcode element selection (public API)
// ---------------------------------------------------------------------------

auto transcode_decoder_alternatives(codec in_codec) -> std::vector<std::string>
{
  switch (in_codec) {
    case codec::av1:
      return {"vaav1dec", "av1dec", "dav1ddec"};
    case codec::h265:
      return {"vah265dec", "avdec_h265"};
    case codec::h264:
      return {"vah264dec", "avdec_h264"};
  }
  return {};
}

auto transcode_source_parser_alternatives(codec in_codec)
    -> std::vector<std::string>
{
  switch (in_codec) {
    case codec::av1:
      return {"av1parse"};
    case codec::h265:
      return {"h265parse"};
    case codec::h264:
      return {"h264parse"};
  }
  return {};
}

auto transcode_encoder_alternatives(transcode_target target)
    -> std::vector<std::string>
{
  // HARDWARE ONLY. Production re-encode must be real, not nominal: a silent
  // software fallback would "work" on a node with no usable GPU while burning the
  // CPU it needs for ingest and fan-out, and the output would be attributed to the
  // tier that never ran. There is deliberately no x264enc/x265enc/av1enc here --
  // if no hardware encoder is present the preflight refuses the /start, which the
  // operator can see, instead of degrading invisibly.
  switch (target) {
    case transcode_target::h264:
      return {"vah264enc"};
    case transcode_target::h265:
      return {"vah265enc"};
    case transcode_target::av1:
      // Not every GPU encodes AV1; where it does not, an AV1 target is refused.
      return {"vaav1enc"};
    case transcode_target::none:
      return {};
  }
  return {};
}

auto transcode_target_parser(transcode_target target) -> const char*
{
  switch (target) {
    case transcode_target::h264:
      return "h264parse";
    case transcode_target::h265:
      return "h265parse";
    case transcode_target::av1:
      return "av1parse";
    case transcode_target::none:
      return nullptr;
  }
  return nullptr;
}

auto transcode_muxer(output_proto proto, transcode_target target) -> const char*
{
  if (target == transcode_target::none) {
    return nullptr;
  }
  switch (proto) {
    case output_proto::rtmp:
    case output_proto::rtmps:
      return target == transcode_target::h265 ? "eflvmux" : "flvmux";
    case output_proto::srt:
    case output_proto::rist:
      return "mpegtsmux";
  }
  return nullptr;
}

auto choose_present(const std::vector<std::string>& alternatives,
                    const element_present_fn& present)
    -> std::optional<std::string>
{
  for (const std::string& name : alternatives) {
    if (present(name.c_str())) {
      return name;
    }
  }
  return std::nullopt;
}

auto required_elements(output_proto proto,
                       const transcode_config& transcode,
                       codec in_codec) -> std::vector<element_requirement>
{
  const transcode_target target = transcode.target;
  const bool transcode_on = target != transcode_target::none;

  std::vector<element_requirement> reqs;
  switch (proto) {
    case output_proto::rtmp:
    case output_proto::rtmps:
      reqs = {{{"appsrc"}, false},
              {{"tsparse"}, false},
              {{"tsdemux"}, false},
              {{"queue"}, false},
              {{"aacparse"}, false},
              {{"rtmp2sink"}, false}};
      // Muxer + video parser: legacy FLV/h264 by default; Enhanced FLV
      // (eflvmux) + h265parse for an h265 transcode target (legacy flvmux
      // cannot carry H.265).
      if (target == transcode_target::h265) {
        reqs.push_back({{"eflvmux"}, true});
        reqs.push_back({{"h265parse"}, true});
      } else {
        reqs.push_back({{"flvmux"}, false});
        reqs.push_back({{"h264parse"}, transcode_on});
      }
      break;
    case output_proto::srt:
      reqs =
          {{{"appsrc"}, false}, {{"tsparse"}, false}, {{"srtsink"}, false}};
      if (transcode_on) {
        reqs.push_back({{"mpegtsmux"}, true});
      }
      break;
    case output_proto::rist:
      reqs =
          {{{"appsrc"}, false}, {{"tsparse"}, false}, {{"ristsink"}, false}};
      if (transcode_on) {
        reqs.push_back({{"mpegtsmux"}, true});
      }
      break;
  }

  if (!transcode_on) {
    return reqs;  // copy-only: the base chain is all that is needed
  }

  // Shared decode chain (one stage, N outputs) plus the target encoder/parser.
  reqs.push_back({transcode_source_parser_alternatives(in_codec), true});
  reqs.push_back({{"tsdemux"}, true});
  reqs.push_back({{"queue"}, true});
  reqs.push_back({{"videoconvert"}, true});
  reqs.push_back({{"videoscale"}, true});
  reqs.push_back({{"capsfilter"}, true});
  reqs.push_back({transcode_decoder_alternatives(in_codec), true});
  reqs.push_back({transcode_encoder_alternatives(target), true});
  reqs.push_back({{transcode_target_parser(target)}, true});
  return reqs;
}

auto validate_transcode_targets(const receiver_config& cfg,
                                bool allow_enhanced_rtmp) -> validation_result
{
  for (std::size_t idx = 0; idx < cfg.outputs.size(); ++idx) {
    const output_config& out = cfg.outputs[idx];
    const bool is_rtmp = out.type == output_proto::rtmp
        || out.type == output_proto::rtmps;

    // ---- Structural. Independent of --allow-enhanced-rtmp, because no policy
    // flag can conjure a muxer that does not exist. ----
    // flvmux and eflvmux expose no video/x-av1 caps even in GStreamer 1.28, so an
    // av1 -> rtmp/rtmps output has nothing to be built with. Refusing here keeps
    // the failure at /start instead of pipeline negotiation.
    if (is_rtmp && out.transcode.target == transcode_target::av1) {
      return fail("bad_enum",
                  std::format("outputs[{}].transcode.codec", idx),
                  "av1 -> rtmp/rtmps is not supported (no video/x-av1 in "
                  "flvmux/eflvmux); send av1 over srt or rist");
    }
    // A scale needs an encoder to apply it. A copy output is a passthrough, so
    // dropping the scale silently would hand the destination a resolution the
    // operator did not ask for and could not see.
    const bool wants_scale =
        out.transcode.scale_width > 0 || out.transcode.scale_height > 0;
    if (wants_scale && out.transcode.target == transcode_target::none) {
      return fail("bad_enum",
                  std::format("outputs[{}].transcode.scale", idx),
                  "scale requires a transcode target: a copy output is a "
                  "passthrough and cannot be rescaled");
    }
    if (wants_scale
        && (out.transcode.scale_width <= 0 || out.transcode.scale_height <= 0)) {
      return fail("bad_enum",
                  std::format("outputs[{}].transcode.scale", idx),
                  "scale needs BOTH width and height");
    }

    // ---- Policy. ----
    if (!allow_enhanced_rtmp && is_rtmp
        && out.transcode.target == transcode_target::h265) {
      return fail("bad_enum",
                  std::format("outputs[{}].transcode.codec", idx),
                  "h265 -> rtmp/rtmps requires --allow-enhanced-rtmp "
                  "(Enhanced FLV muxer, eflvmux)");
    }
  }
  return {};
}


auto first_missing_requirement(const std::vector<element_requirement>& reqs,
                               const element_present_fn& present)
    -> std::optional<missing_requirement>
{
  for (const element_requirement& req : reqs) {
    const bool satisfied = std::ranges::any_of(
        req.names, [&present](const std::string& name) -> bool
        { return present(name.c_str()); });
    if (!satisfied) {
      return missing_requirement {req.names.front(), req.transcode};
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Egress policy (public API)
// ---------------------------------------------------------------------------

auto default_resolver() -> resolve_fn
{
  return [](const std::string& host, std::vector<std::string>& ips) -> bool
  {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(unbracket(host).c_str(), nullptr, &hints, &results) != 0) {
      return false;
    }
    for (const addrinfo* cur = results; cur != nullptr; cur = cur->ai_next) {
      std::array<char, INET6_ADDRSTRLEN> text {};
      if (cur->ai_family == AF_INET) {
        const auto* sin =
            reinterpret_cast<const sockaddr_in*>(cur->ai_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, text.data(), text.size())
            != nullptr)
        {
          ips.emplace_back(text.data());
        }
      } else if (cur->ai_family == AF_INET6) {
        const auto* sin6 =
            reinterpret_cast<const sockaddr_in6*>(cur->ai_addr);
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, text.data(), text.size())
            != nullptr)
        {
          ips.emplace_back(text.data());
        }
      }
    }
    freeaddrinfo(results);
    return !ips.empty();
  };
}

auto forbidden_reason(const std::string& ip_literal,
                      const egress_policy& policy) -> const char*
{
  const parsed_ip ip_addr = parse_ip(ip_literal);
  if (!ip_addr.valid) {
    return "unparsable";
  }
  if (const char* reason = classify(ip_addr, policy.allow_private);
      reason != nullptr)
  {
    return reason;
  }
  for (const std::string& text : policy.deny_cidrs) {
    cidr net;
    if (parse_cidr(text, net) && cidr_contains(net, ip_addr)) {
      return "denylist";
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Validation entry points (public API)
// ---------------------------------------------------------------------------

auto validate_config(receiver_config& cfg) -> validation_result
{
  if (cfg.schema_version != k_schema_version) {
    return fail("invalid_schema",
                "schema_version",
                "unsupported schema_version (expected 3)");
  }

  if (const auto res = validate_ingest(cfg.ingest); !res.ok) {
    return res;
  }

  if (cfg.outputs.size() > k_max_outputs) {
    return fail("out_of_range",
                "outputs",
                std::format("at most {} outputs", k_max_outputs));
  }

  std::unordered_set<std::string> seen_ids;
  bool any_copy_rtmp = false;
  for (std::size_t idx = 0; idx < cfg.outputs.size(); ++idx) {
    output_config& out = cfg.outputs[idx];
    const std::string field = std::format("outputs[{}]", idx);

    if (out.id.empty()) {
      return fail("bad_enum", field + ".id", "output id must be non-empty");
    }
    if (!seen_ids.insert(out.id).second) {
      return fail("bad_enum", "outputs[].id", "duplicate output id: " + out.id);
    }

    if (scheme_of(out.url) != expected_scheme(out.type)) {
      return fail("bad_url",
                  field + ".url",
                  std::format("URL scheme must match type \"{}\"",
                              to_string(out.type)));
    }
    if (!is_pipeline_safe(out.url) || !is_pipeline_safe(out.key_or_streamid)) {
      return fail("bad_url",
                  field + ".url",
                  "URL/key contains quote-escape or control characters");
    }
    if (!split_url(out.url, out.host, out.port, out.path)) {
      return fail("bad_url", field + ".url", "unparsable URL authority");
    }
    if (out.port == 0) {
      out.port = default_port(out.type);
      if (out.port == 0) {
        return fail("bad_url",
                    field + ".url",
                    "srt/rist URLs require an explicit port");
      }
    }

    // Only a COPY-only rtmp/rtmps output constrains the source codec: FLV
    // needs AVC. An rtmp output that opts into transcode accepts any source
    // (the pipeline decodes + re-encodes to its target).
    const bool is_rtmp = out.type == output_proto::rtmp
        || out.type == output_proto::rtmps;
    if (is_rtmp && out.transcode.target == transcode_target::none) {
      any_copy_rtmp = true;
    }
  }

  // Fail early on the declared hint (TRANSPORT_PROFILE §1.2): copy-only fan-out
  // can put only H.264 into FLV. The runtime-detection analogue (a non-H264 ES
  // appearing mid-stream) degrades just the rtmp outputs, not the session.
  // srt/rist outputs and transcode-enabled rtmp outputs are unaffected.
  if (any_copy_rtmp && cfg.in_codec != codec::h264) {
    return fail("rtmp_codec_unsupported",
                "source.codec",
                "rtmp/rtmps outputs require h264 unless they transcode");
  }

  return {};
}

auto validate_and_pin_outputs(receiver_config& cfg,
                              const egress_policy& policy,
                              const resolve_fn& resolve) -> validation_result
{
  for (output_config& out : cfg.outputs) {
    // A literal-IP host classifies directly; otherwise resolve and vet EVERY
    // returned address (a rebinding name must not pass because one A record is
    // public), then pin the first acceptable one. The pipeline connects to
    // pinned_ip and never re-resolves (M1.2).
    std::vector<std::string> ips;
    if (parse_ip(unbracket(out.host)).valid) {
      ips.push_back(unbracket(out.host));
    } else {
      if (!resolve || !resolve(out.host, ips) || ips.empty()) {
        return fail("bad_url",
                    "outputs[].url",
                    "destination host did not resolve (output " + out.id + ")");
      }
    }
    for (const std::string& ip_literal : ips) {
      if (forbidden_reason(ip_literal, policy) != nullptr) {
        // Name the output id only — never the resolved IP (topology leak).
        return fail("forbidden_destination",
                    "outputs[].url",
                    "destination not permitted for output " + out.id);
      }
    }
    out.pinned_ip = ips.front();
  }
  return {};
}
