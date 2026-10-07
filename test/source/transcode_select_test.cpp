// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the transcode element-selection logic (MT1.6/MT1.7): the
// alternatives lists shared by the /start preflight and the runtime decoder
// and output builders, the muxer rule (flvmux / eflvmux / mpegtsmux), and the
// --allow-enhanced-rtmp gate. Framework-free; exit 0 = pass. No GStreamer.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
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

// A present-fn stub: exactly the named elements "exist".
auto only(const std::vector<std::string>& present)
{
  return [present](const char* name) -> bool
  {
    return std::ranges::find(present, std::string {name}) != present.end();
  };
}
}  // namespace

auto main() -> int
{
  // Encoder alternatives: HARDWARE ONLY. There is deliberately no software
  // fallback -- production does not use software encoding, and a silent CPU
  // re-encode would be attributed to a GPU tier that never ran.
  {
    const auto h264 = transcode_encoder_alternatives(transcode_target::h264);
    expect(h264 == (std::vector<std::string> {"vah264enc"}),
           "h264 encoder is hardware only");
    const auto h265 = transcode_encoder_alternatives(transcode_target::h265);
    expect(h265 == (std::vector<std::string> {"vah265enc"}),
           "h265 encoder is hardware only");
    const auto av1 = transcode_encoder_alternatives(transcode_target::av1);
    expect(av1 == (std::vector<std::string> {"vaav1enc"}),
           "av1 encoder is hardware only");
    expect(transcode_encoder_alternatives(transcode_target::none).empty(),
           "none has no encoder");
  }

  // Decoder + source-parser alternatives (hardware first).
  {
    const auto av1 = transcode_decoder_alternatives(codec::av1);
    expect(!av1.empty() && av1.front() == "vaav1dec",
           "av1 decoder prefers vaav1dec");
    expect(transcode_decoder_alternatives(codec::h265).front() == "vah265dec",
           "h265 decoder prefers vah265dec");
    expect(transcode_decoder_alternatives(codec::h264).front() == "vah264dec",
           "h264 decoder prefers vah264dec");

    expect(transcode_source_parser_alternatives(codec::av1)
               == std::vector<std::string> {"av1parse"},
           "av1 source parser");
    expect(transcode_source_parser_alternatives(codec::h265)
               == std::vector<std::string> {"h265parse"},
           "h265 source parser");
    expect(transcode_source_parser_alternatives(codec::h264)
               == std::vector<std::string> {"h264parse"},
           "h264 source parser");
  }

  // Target parser + muxer rule.
  {
    expect(std::string {transcode_target_parser(transcode_target::h264)}
               == "h264parse",
           "h264 target parser");
    expect(std::string {transcode_target_parser(transcode_target::h265)}
               == "h265parse",
           "h265 target parser");
    expect(transcode_target_parser(transcode_target::none) == nullptr,
           "none has no target parser");

    expect(std::string {transcode_muxer(output_proto::rtmp,
                                        transcode_target::h264)}
               == "flvmux",
           "h264 rtmp -> flvmux");
    expect(std::string {transcode_muxer(output_proto::rtmp,
                                        transcode_target::h265)}
               == "eflvmux",
           "h265 rtmp -> eflvmux (Enhanced FLV)");
    expect(std::string {transcode_muxer(output_proto::srt,
                                        transcode_target::h264)}
               == "mpegtsmux",
           "h264 srt -> mpegtsmux");
    expect(std::string {transcode_muxer(output_proto::rist,
                                        transcode_target::h265)}
               == "mpegtsmux",
           "h265 rist -> mpegtsmux");
    expect(transcode_muxer(output_proto::rtmp, transcode_target::none)
               == nullptr,
           "copy-only has no transcode muxer");
  }

  // choose_present picks the first available, else nullopt.
  {
    const auto alts =
        std::vector<std::string> {"vah264enc", "x264enc"};
    expect(choose_present(alts, only({"vah264enc", "x264enc"}))
               == std::optional<std::string> {"vah264enc"},
           "first present alternative is chosen");
    expect(choose_present(alts, only({"x264enc"}))
               == std::optional<std::string> {"x264enc"},
           "a later alternative is chosen when earlier ones are absent");
    expect(!choose_present(alts, only({"nope"})).has_value(),
           "none present -> nullopt");
  }

  // required_elements: h265 transcode rtmp requires h265parse + eflvmux (not
  // legacy flvmux), and the decode chain; copy-only does not.
  {
    transcode_config tc;
    tc.target = transcode_target::h265;
    const auto reqs =
        required_elements(output_proto::rtmp, tc, codec::av1);
    const auto has = [&reqs](const std::string& name) -> bool
    {
      return std::ranges::any_of(
          reqs, [&name](const element_requirement& req) -> bool
          { return std::ranges::find(req.names, name) != req.names.end(); });
    };
    expect(has("eflvmux") && has("h265parse"), "h265 rtmp mux/parser");
    expect(!has("flvmux"), "h265 rtmp does not require legacy flvmux");
    expect(has("vaav1dec") && has("av1parse"), "decode chain required");

    const auto copy_reqs =
        required_elements(output_proto::rtmp, transcode_config {}, codec::av1);
    const auto copy_has = [&copy_reqs](const std::string& name) -> bool
    {
      return std::ranges::any_of(
          copy_reqs, [&name](const element_requirement& req) -> bool
          { return std::ranges::find(req.names, name) != req.names.end(); });
    };
    expect(copy_has("flvmux") && !copy_has("eflvmux"),
           "copy-only rtmp keeps legacy flvmux");
    expect(!copy_has("videoconvert"), "copy-only needs no decode chain");
  }

  // --allow-enhanced-rtmp gate (pure).
  {
    receiver_config cfg;
    cfg.session_id = "s1";
    output_config out;
    out.id = "yt";
    out.type = output_proto::rtmp;
    out.url = "rtmp://a.rtmp.example.com/live2";
    out.transcode.target = transcode_target::h265;
    cfg.outputs.push_back(out);
    const auto without = validate_transcode_targets(cfg, false);
    expect(!without.ok && without.error_code == "bad_enum"
               && without.field == "outputs[0].transcode.codec",
           "h265 rtmp without flag -> bad_enum on transcode.codec");
    expect(validate_transcode_targets(cfg, true).ok,
           "h265 rtmp with flag is allowed");

    cfg.outputs[0].transcode.target = transcode_target::h264;
    expect(validate_transcode_targets(cfg, false).ok,
           "h264 rtmp never needs the flag");
  }

  std::puts("transcode_select_test: ALL OK");
  return 0;
}
