// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for schema-2 structural validation and M1.2 egress validation
// (destination classes, deny-list, DNS-rebinding resistance, IP pinning).
// Framework-free; exit 0 = pass.

#include <algorithm>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "lib/lib.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto make_output(std::string oid,
                 output_proto proto,
                 std::string url,
                 std::string key) -> output_config
{
  output_config out;
  out.id = std::move(oid);
  out.type = proto;
  out.url = std::move(url);
  out.key_or_streamid = std::move(key);
  return out;
}

auto make_cfg() -> receiver_config
{
  receiver_config cfg;
  cfg.session_id = "t1";
  cfg.outputs.push_back(make_output(
      "yt", output_proto::rtmp, "rtmp://a.rtmp.example.com/live2", "sekret"));
  return cfg;
}

// ---------------------------------------------------------------------------
auto test_structural() -> void
{
  {
    receiver_config cfg = make_cfg();
    expect(validate_config(cfg).ok, "valid config passes");
    expect(cfg.outputs[0].host == "a.rtmp.example.com", "host parsed");
    expect(cfg.outputs[0].port == 1935, "rtmp default port");
    expect(cfg.outputs[0].path == "/live2", "path parsed");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.schema_version = 1;
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "invalid_schema", "v1 body rejected");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.outputs.push_back(cfg.outputs[0]);  // duplicate id
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "bad_enum"
               && res.field == "outputs[].id",
           "duplicate id -> bad_enum outputs[].id");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.outputs[0].type = output_proto::srt;  // scheme/type mismatch
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "bad_url", "scheme/type mismatch");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.outputs[0] =
        make_output("cli", output_proto::srt, "srt://203.0.113.9", "s");
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "bad_url", "srt needs explicit port");
  }
  {
    receiver_config cfg = make_cfg();
    for (int idx = 0; idx < 8; ++idx) {
      output_config out = cfg.outputs[0];
      out.id = "o" + std::to_string(idx);
      cfg.outputs.push_back(out);
    }
    const auto res = validate_config(cfg);  // 9 outputs
    expect(!res.ok && res.error_code == "out_of_range", "max 8 outputs");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.in_codec = codec::h265;
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "rtmp_codec_unsupported",
           "h265 + rtmp fails early");
    cfg.outputs[0] =
        make_output("cli", output_proto::srt, "srt://203.0.113.9:9000", "s");
    expect(validate_config(cfg).ok, "h265 + srt-only is fine");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.outputs[0].key_or_streamid = "bad'key";
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "bad_url", "quote in key rejected");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.outputs.clear();  // empty outputs = legal link-test session
    expect(validate_config(cfg).ok, "empty outputs[] is legal");
  }
  std::puts("ok: structural");
}

// ---------------------------------------------------------------------------
auto test_ip_classes() -> void
{
  const egress_policy strict {};
  const egress_policy lan {.allow_private = true, .deny_cidrs = {}};

  expect(forbidden_reason("169.254.169.254", strict) != nullptr,
         "metadata IP forbidden");
  expect(forbidden_reason("127.0.0.1", strict) != nullptr, "loopback");
  expect(forbidden_reason("10.0.0.5", strict) != nullptr, "RFC1918 10/8");
  expect(forbidden_reason("172.16.9.1", strict) != nullptr, "RFC1918 172.16/12");
  expect(forbidden_reason("192.168.1.10", strict) != nullptr, "RFC1918 192.168/16");
  expect(forbidden_reason("100.64.0.1", strict) != nullptr, "CGNAT 100.64/10");
  expect(forbidden_reason("224.0.0.1", strict) != nullptr, "v4 multicast");
  expect(forbidden_reason("::1", strict) != nullptr, "v6 loopback");
  expect(forbidden_reason("fe80::1", strict) != nullptr, "v6 link-local");
  expect(forbidden_reason("fd00::1", strict) != nullptr, "ULA");
  expect(forbidden_reason("ff02::1", strict) != nullptr, "v6 multicast");
  expect(forbidden_reason("::ffff:169.254.169.254", strict) != nullptr,
         "v4-mapped metadata blocked");
  expect(forbidden_reason("203.0.113.9", strict) == nullptr, "public v4 ok");
  expect(forbidden_reason("2001:db8::9", strict) == nullptr, "public v6 ok");

  // LAN opt-out: private allowed; loopback/link-local/multicast still not.
  expect(forbidden_reason("10.0.0.5", lan) == nullptr, "opt-out allows RFC1918");
  expect(forbidden_reason("fd00::1", lan) == nullptr, "opt-out allows ULA");
  expect(forbidden_reason("169.254.169.254", lan) != nullptr,
         "opt-out still blocks metadata");
  expect(forbidden_reason("127.0.0.1", lan) != nullptr,
         "opt-out still blocks loopback");

  // Deny-list.
  const egress_policy denyl {.allow_private = false,
                             .deny_cidrs = {"203.0.113.0/24", "2001:db8::/32"}};
  expect(forbidden_reason("203.0.113.9", denyl) != nullptr, "cidr denylist v4");
  expect(forbidden_reason("2001:db8::9", denyl) != nullptr, "cidr denylist v6");
  expect(forbidden_reason("198.51.100.9", denyl) == nullptr,
         "outside denylist ok");
  std::puts("ok: ip classes");
}

// ---------------------------------------------------------------------------
auto test_egress_pinning() -> void
{
  const egress_policy strict {};

  // Rebinding resolver stub: same name yields public on call 1, private on
  // call 2 (classic TOCTOU flip). Because validation vets and PINS the first
  // resolution's IPs and the pipeline connects to the pinned IP without
  // re-resolving, the flip has nothing to attack. And if ANY returned record
  // is private (dual A response), validation must reject outright.
  int calls = 0;
  const resolve_fn rebinding =
      [&calls](const std::string& host, std::vector<std::string>& ips) -> bool
  {
    (void)host;
    ++calls;
    if (calls == 1) {
      ips = {"203.0.113.50"};
    } else {
      ips = {"10.0.0.5"};
    }
    return true;
  };

  {
    receiver_config cfg = make_cfg();
    expect(validate_config(cfg).ok, "structural ok");
    const auto res = validate_and_pin_outputs(cfg, strict, rebinding);
    expect(res.ok, "first resolution (public) accepted");
    expect(cfg.outputs[0].pinned_ip == "203.0.113.50",
           "vetted IP pinned for connect");
    // The later 'resolution' flips private, but nothing re-resolves: the
    // pinned IP is the connect target for the output's whole lifetime.
  }
  {
    // Dual-record response containing a private address → rejected.
    const resolve_fn dual =
        [](const std::string&, std::vector<std::string>& ips) -> bool
    {
      ips = {"203.0.113.50", "10.0.0.5"};
      return true;
    };
    receiver_config cfg = make_cfg();
    expect(validate_config(cfg).ok, "structural ok");
    const auto res = validate_and_pin_outputs(cfg, strict, dual);
    expect(!res.ok && res.error_code == "forbidden_destination",
           "any private record rejects the destination");
    expect(res.message.find("10.0.0.5") == std::string::npos,
           "resolved IP never leaks in the error");
    expect(res.message.find("yt") != std::string::npos,
           "error names the output id");
  }
  {
    // Literal-IP URLs classify directly (M1.2 acceptance cases).
    receiver_config cfg = make_cfg();
    cfg.outputs[0] = make_output(
        "meta", output_proto::rtmp, "rtmp://169.254.169.254/x", "k");
    expect(validate_config(cfg).ok, "structural ok for literal");
    const auto res = validate_and_pin_outputs(cfg, strict, default_resolver());
    expect(!res.ok && res.error_code == "forbidden_destination",
           "rtmp://169.254.169.254 rejected");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.outputs[0] =
        make_output("lanbox", output_proto::srt, "srt://10.0.0.5:9000", "s");
    expect(validate_config(cfg).ok, "structural ok");
    const auto strict_res =
        validate_and_pin_outputs(cfg, strict, default_resolver());
    expect(!strict_res.ok && strict_res.error_code == "forbidden_destination",
           "srt://10.0.0.5 rejected by default");

    const egress_policy lan {.allow_private = true, .deny_cidrs = {}};
    const auto lan_res = validate_and_pin_outputs(cfg, lan, default_resolver());
    expect(lan_res.ok, "LAN case passes with --egress-allow-private");
    expect(cfg.outputs[0].pinned_ip == "10.0.0.5", "literal pinned");
  }
  std::puts("ok: egress pinning + rebinding");
}

// ---------------------------------------------------------------------------
auto test_transcode() -> void
{
  // transcode_target enum <-> string round-trip + reject an unknown codec.
  {
    transcode_target tgt = transcode_target::none;
    expect(parse_transcode_target("h264", tgt), "parse transcode h264");
    expect(tgt == transcode_target::h264, "h264 target value");
    expect(parse_transcode_target("h265", tgt)
               && tgt == transcode_target::h265,
           "parse transcode h265");
    expect(!parse_transcode_target("vp9", tgt), "vp9 rejected as target");
    expect(!parse_transcode_target("none", tgt),
           "none rejected (copy = omit object)");
    expect(std::string(to_string(transcode_target::h264)) == "h264",
           "to_string transcode h264");
    expect(std::string(to_string(transcode_target::none)) == "none",
           "to_string transcode none");
  }

  // rtmp codec gate is transcode-aware (MT1.3).
  {
    receiver_config cfg = make_cfg();
    cfg.in_codec = codec::av1;
    cfg.outputs[0].transcode.target = transcode_target::h264;
    expect(validate_config(cfg).ok, "av1 + transcode rtmp is legal");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.in_codec = codec::av1;  // rtmp copy output, no transcode
    const auto res = validate_config(cfg);
    expect(!res.ok && res.error_code == "rtmp_codec_unsupported"
               && res.field == "source.codec",
           "av1 + copy rtmp -> rtmp_codec_unsupported");
  }
  {
    receiver_config cfg = make_cfg();
    cfg.in_codec = codec::av1;
    cfg.outputs[0] =
        make_output("cli", output_proto::srt, "srt://203.0.113.9:9000", "s");
    expect(validate_config(cfg).ok, "av1 + srt-only is fine");
  }

  // Element availability for an av1 -> h264 transcode output (MT1.4).
  {
    transcode_config tcfg;
    tcfg.target = transcode_target::h264;
    const auto reqs = required_elements(output_proto::rtmp, tcfg, codec::av1);
    const auto has = [&reqs](const std::string& name) -> bool
    {
      return std::ranges::any_of(
          reqs, [&name](const element_requirement& req) -> bool
          { return std::ranges::find(req.names, name) != req.names.end(); });
    };
    expect(has("tsdemux") && has("videoconvert") && has("videoscale")
               && has("capsfilter"),
           "convert chain elements required");
    expect(has("vaav1dec") && has("av1dec") && has("dav1ddec"),
           "av1 decoder alternatives listed");
    expect(has("vah264enc") && !has("x264enc"),
           "h264 encoder requirement is hardware only");
    expect(has("h264parse"), "target parser required");

    // ANY alternative satisfies presence.
    const element_present_fn everything = [](const char*) -> bool
    { return true; };
    expect(!first_missing_requirement(reqs, everything).has_value(),
           "all elements present -> satisfied");

    // Hardware encoder present -> satisfied.
    const element_present_fn hw_only = [](const char* name) -> bool
    { return std::string_view {name} != "x264enc"; };
    expect(!first_missing_requirement(reqs, hw_only).has_value(),
           "vah264enc satisfies the encoder requirement");

    // No hardware encoder -> missing, named and flagged as a transcode-chain
    // element, so the /start is refused rather than served by software.
    const element_present_fn no_encoder = [](const char* name) -> bool
    {
      const std::string_view elem {name};
      return elem != "vah264enc";
    };
    const auto miss = first_missing_requirement(reqs, no_encoder);
    expect(miss.has_value() && miss->element == "vah264enc"
               && miss->transcode,
           "both encoders missing -> transcode_unavailable(vah264enc)");

    // A copy-only output must not require the transcode chain.
    const auto copy_reqs =
        required_elements(output_proto::rtmp, transcode_config {}, codec::av1);
    const element_present_fn no_chain = [](const char* name) -> bool
    {
      const std::string_view elem {name};
      return elem != "videoconvert" && elem != "vah264enc"
          && elem != "x264enc";
    };
    expect(!first_missing_requirement(copy_reqs, no_chain).has_value(),
           "copy output does not need the transcode chain");
  }
  std::puts("ok: transcode");
}
}  // namespace

auto main() -> int
{
  test_structural();
  test_ip_classes();
  test_egress_pinning();
  test_transcode();
  std::puts("validate_test: ALL OK");
  return 0;
}
