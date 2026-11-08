// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Unit tests for the POST /start body parser (schema_version 5: the optional
// `standby` object and the source geometry; 4 is still accepted transitionally).
// Framework-free; exit 0 = pass.

#include <cstdio>
#include <cstdlib>
#include <format>
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

// One rtmp output, with `transcode_json` spliced into it when non-empty.
auto output_json(const std::string& transcode_json) -> std::string
{
  std::string out_json =
      "{\"id\":\"yt\",\"type\":\"rtmp\","
      "\"url\":\"rtmp://a.rtmp.example.com/live2\","
      "\"key_or_streamid\":\"sekret\"";
  if (!transcode_json.empty()) {
    out_json += "," + transcode_json;
  }
  out_json += "}";
  return out_json;
}

// A whole /start body. `source_json` defaults to av1; `standby_json` is spliced
// in only when non-empty.
auto make_body(int version,
               const std::string& standby_json = "",
               const std::string& source_json = "{\"codec\":\"av1\"}",
               const std::string& transcode_json = "") -> std::string
{
  std::string body = std::format(
      "{{\"schema_version\":{},\"session_id\":\"s1\",\"source\":{},",
      version,
      source_json);
  if (!standby_json.empty()) {
    body += "\"standby\":" + standby_json + ",";
  }
  body += "\"outputs\":[" + output_json(transcode_json) + "]}";
  return body;
}

auto body_with_transcode(const std::string& transcode_json) -> std::string
{
  return make_body(k_schema_version, "", "{\"codec\":\"av1\"}", transcode_json);
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

  // A body at any other schema_version is an invalid_schema error that names
  // the version this build speaks.
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
    expect(msg
               == std::format("unsupported schema_version (expected {})",
                              k_schema_version),
           "the rejection names the schema_version this build accepts");
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

  // schema_version 5: an absent `standby` object leaves every default intact.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(k_schema_version);
    expect(parse_start_body(body, cfg, code, field, msg),
           "v5 body without standby parses");
    expect(!cfg.standby.enabled, "absent standby => enabled false");
    expect(cfg.standby.width == 0 && cfg.standby.height == 0
               && cfg.standby.fps == 0,
           "standby geometry defaults to 0 (unknown)");
    expect(cfg.standby.bitrate_kbps == 1500, "standby.bitrate_kbps default");
    expect(cfg.standby.max_live_gap_s == 1800, "standby.max_live_gap_s default");
    expect(cfg.standby.live_gap_ms == 1500, "standby.live_gap_ms default");
    expect(cfg.standby.live_hold_ms == 500, "standby.live_hold_ms default");
    expect(cfg.standby.max_gap_ms == 1000, "standby.max_gap_ms default");
    expect(cfg.standby.brand_image.empty(), "standby.brand_image default empty");
    expect(cfg.standby.text == "stream standby", "standby.text default");
  }

  // schema_version 5: a `standby` object parses field by field.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(
        k_schema_version,
        "{\"enabled\":true,\"max_live_gap_s\":60,\"bitrate_kbps\":2000}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "v5 body with standby parses");
    expect(cfg.standby.enabled, "standby.enabled parsed");
    expect(cfg.standby.max_live_gap_s == 60, "standby.max_live_gap_s parsed");
    expect(cfg.standby.bitrate_kbps == 2000, "standby.bitrate_kbps parsed");
    // Unnamed members keep their defaults.
    expect(cfg.standby.live_gap_ms == 1500 && cfg.standby.max_gap_ms == 1000,
           "unset standby members keep their defaults");
  }

  // A string where a boolean belongs is a schema error, not a truthy guess.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body =
        make_body(k_schema_version, "{\"enabled\":\"yes\"}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "string standby.enabled rejected");
    expect(code == "invalid_schema", "string enabled -> invalid_schema");
    expect(field == "standby.enabled", "names standby.enabled");
    expect(!cfg.standby.enabled, "enabled stays false on a parse failure");
  }

  // Out-of-bounds standby numbers are out_of_range, naming the field.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body =
        make_body(k_schema_version, "{\"max_live_gap_s\":-1}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "negative max_live_gap_s rejected");
    expect(code == "out_of_range", "negative gap -> out_of_range");
    expect(field == "standby.max_live_gap_s", "names standby.max_live_gap_s");
  }

  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body =
        make_body(k_schema_version, "{\"bitrate_kbps\":10}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "bitrate_kbps below the 100 floor rejected");
    expect(code == "out_of_range", "low bitrate -> out_of_range");
    expect(field == "standby.bitrate_kbps", "names standby.bitrate_kbps");
  }

  // brand_image is a path INSIDE the container: absolute or empty, nothing else.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(
        k_schema_version, "{\"brand_image\":\"relative/path.png\"}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "relative brand_image rejected");
    expect(code == "invalid_schema", "relative brand_image -> invalid_schema");
    expect(field == "standby.brand_image", "names standby.brand_image");
  }

  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(
        k_schema_version, "{\"brand_image\":\"/usr/share/obr/brand.png\"}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "absolute brand_image accepted");
    expect(cfg.standby.brand_image == "/usr/share/obr/brand.png",
           "brand_image parsed");
  }

  // Source geometry (schema_version 5): the arriving picture, so a slate or a
  // standby source can match it.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(
        k_schema_version, "", "{\"codec\":\"h264\",\"width\":1920,"
                              "\"height\":1080,\"fps\":60}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "v5 body with source geometry parses");
    expect(cfg.in_codec == codec::h264, "source.codec still parsed");
    expect(cfg.source.width == 1920, "source.width parsed");
    expect(cfg.source.height == 1080, "source.height parsed");
    expect(cfg.source.fps == 60, "source.fps parsed");
  }

  // Absent geometry means unknown (0), never a guessed size.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    expect(parse_start_body(make_body(k_schema_version), cfg, code, field, msg),
           "v5 body without geometry parses");
    expect(cfg.source.width == 0 && cfg.source.height == 0
               && cfg.source.fps == 0,
           "absent geometry => 0");
  }

  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(
        k_schema_version, "", "{\"codec\":\"h264\",\"width\":\"1920\"}");
    expect(!parse_start_body(body, cfg, code, field, msg),
           "string source.width rejected");
    expect(code == "invalid_schema", "string width -> invalid_schema");
    expect(field == "source.width", "names source.width");
  }

  // Transitional: a body that declares 4 is still accepted -- the encoder lives
  // in another repository and still sends 4. A v4 body cannot express standby,
  // so standby is forced off whatever the block says.
  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body =
        make_body(4, "{\"enabled\":true,\"bitrate_kbps\":2000}");
    expect(parse_start_body(body, cfg, code, field, msg),
           "v4 body with a standby block is accepted");
    expect(!cfg.standby.enabled, "v4 => standby.enabled forced false");
  }

  {
    receiver_config cfg;
    std::string code;
    std::string field;
    std::string msg;
    const std::string body = make_body(4);
    expect(parse_start_body(body, cfg, code, field, msg),
           "v4 body without standby is accepted");
    expect(!cfg.standby.enabled, "v4 => standby.enabled false");
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
