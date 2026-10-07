// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the POST /start body parser (schema_version 4: the optional
// outputs[].transcode object, its `scale` block, and the av1 target).
// Framework-free; exit 0 = pass.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "control/control.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// Minimal v3 body; transcode JSON is spliced in per case.
auto body_with_transcode(const std::string& transcode_json) -> std::string
{
  std::string out_json =
      "{\"id\":\"yt\",\"type\":\"rtmp\","
      "\"url\":\"rtmp://a.rtmp.example.com/live2\","
      "\"key_or_streamid\":\"sekret\"";
  if (!transcode_json.empty()) {
    out_json += "," + transcode_json;
  }
  out_json += "}";
  return std::string {"{\"schema_version\":4,\"session_id\":\"s1\","
                      "\"source\":{\"codec\":\"av1\"},\"outputs\":["}
      + out_json + "]}";
}

// ---------------------------------------------------------------------------
auto test_parse() -> void
{
  // A v3 body whose rtmp output requests an h265 transcode target.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode(
        "\"transcode\":{\"codec\":\"h265\",\"bitrate_kbps\":6000,\"gop\":60}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "v3 body with transcode parses");
    expect(cfg.outputs.size() == 1, "one output parsed");
    expect(cfg.outputs[0].transcode.target == transcode_target::h265,
           "transcode.target == h265");
    expect(cfg.outputs[0].transcode.bitrate_kbps == 6000, "bitrate parsed");
    expect(cfg.outputs[0].transcode.gop == 60, "gop parsed");
  }

  // Absent transcode object => copy-only default.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode("");
    expect(parse_start_body(body, cfg, code, field, msg),
           "v3 copy-only body parses");
    expect(cfg.outputs[0].transcode.target == transcode_target::none,
           "absent transcode => none");
  }

  // Unknown target codec => bad_enum on outputs[0].transcode.codec.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode("\"transcode\":{\"codec\":\"vp9\"}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "vp9 transcode rejected");
    expect(code == "bad_enum", "vp9 -> bad_enum");
    expect(field == "outputs[0].transcode.codec",
           "vp9 -> outputs[0].transcode.codec");
  }

  // schema_version 4: a scale block is parsed into the config (output size is
  // applied before the encoder, so it belongs to the transcode, not the output).
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode(
        "\"transcode\":{\"codec\":\"h264\",\"scale\":{\"width\":3840,\"height\":2160}}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "v4 body with scale parses");
    expect(cfg.outputs[0].transcode.scale_width == 3840, "scale width parsed");
    expect(cfg.outputs[0].transcode.scale_height == 2160, "scale height parsed");
  }

  // av1 IS a target now. (Whether it can reach a given protocol is a separate,
  // structural check -- see validate_test.)
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode("\"transcode\":{\"codec\":\"av1\"}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "av1 transcode target is accepted");
    expect(cfg.outputs[0].transcode.target == transcode_target::av1,
           "transcode.target == av1");
  }

  // A scale that is not an object is a schema error, not a silent ignore.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode(
        "\"transcode\":{\"codec\":\"h264\",\"scale\":3840}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "non-object scale rejected");
    expect(code == "invalid_schema", "non-object scale -> invalid_schema");
  }

  // transcode present but codec missing => invalid_schema on the field.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = body_with_transcode("\"transcode\":{}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "transcode without codec rejected");
    expect(code == "invalid_schema", "missing codec -> invalid_schema");
    expect(field == "outputs[0].transcode.codec", "names the codec field");
  }

  // A non-3 schema_version body is still an invalid_schema error.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body =
        "{\"schema_version\":2,\"session_id\":\"s1\","
        "\"source\":{\"codec\":\"h264\"},\"outputs\":[]}";
    expect(!parse_start_body(body, cfg, code, field, msg),
           "v2 body rejected");
    expect(code == "invalid_schema" && field == "schema_version",
           "v2 -> invalid_schema schema_version");
  }

  // Malformed JSON => invalid_schema.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    expect(!parse_start_body("{not json", cfg, code, field, msg),
           "malformed body rejected");
    expect(code == "invalid_schema", "malformed -> invalid_schema");
  }

  std::puts("ok: start parse");
}
}  // namespace

auto main() -> int
{
  test_parse();
  std::puts("start_parse_test: ALL OK");
  return 0;
}
