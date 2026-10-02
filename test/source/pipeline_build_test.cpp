// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

// Full-lane pipeline-string tests (MT1.6/MT1.7): assert the exact templates
// the decoder and outputs build, and gst_parse_launch them to prove they link.
// Needs GStreamer, so this test is only added outside OBR_TESTS_ONLY.
//
// The rist template is asserted by substring only: in this build `ristsink`
// advertises `application/x-rtp` sink caps, so `mpegtsmux ! ristsink` cannot
// be dynamically linked here (the copy-only rist template has the same
// pre-existing constraint); the transcode rule still emits the specified
// chain.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <gst/gst.h>

#include "fanout/decoder.h"
#include "fanout/output.h"

namespace
{
auto expect(bool cond, const char* what) -> void
{
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

auto has(const std::string& text, std::string_view needle) -> bool
{
  return text.find(needle) != std::string::npos;
}

auto parses(const std::string& tmpl) -> bool
{
  GError* err = nullptr;
  GstElement* pipeline = gst_parse_launch(tmpl.c_str(), &err);
  const bool ok = pipeline != nullptr && err == nullptr;
  if (err != nullptr) {
    std::fprintf(stderr, "  parse error: %s\n", err->message);
    g_error_free(err);
  }
  if (pipeline != nullptr) {
    gst_object_unref(pipeline);
  }
  return ok;
}
}  // namespace

auto main() -> int
{
  gst_init(nullptr, nullptr);

  transcode_config h264;
  h264.target = transcode_target::h264;
  transcode_config h265;
  h265.target = transcode_target::h265;

  // Copy-only templates are unchanged (legacy flvmux, no encoder element).
  {
    const std::string rtmp =
        output_template(output_proto::rtmp, transcode_config {}, nullptr);
    expect(has(rtmp, "flvmux name=mux") && !has(rtmp, "name=venc"),
           "copy rtmp keeps flvmux and has no encoder");
    expect(parses(rtmp), "copy rtmp template parses");

    const std::string srt =
        output_template(output_proto::srt, transcode_config {}, nullptr);
    expect(has(srt, "srtsink name=osink"), "copy srt unchanged");
    expect(parses(srt), "copy srt template parses");
  }

  // h264 transcode -> RTMP: encoder + h264parse + flvmux + AAC audio branch.
  {
    const std::string tmpl =
        output_template(output_proto::rtmp, h264, "vah264enc");
    expect(has(tmpl, "vah264enc name=venc"), "h264 rtmp encoder");
    expect(has(tmpl, "h264parse config-interval=-1"), "h264 rtmp parser");
    expect(has(tmpl, "video/x-h264,stream-format=avc,alignment=au"),
           "h264 rtmp AVC caps");
    expect(has(tmpl, "flvmux name=mux"), "h264 rtmp legacy flvmux");
    expect(has(tmpl, "appsrc name=asrc") && has(tmpl, "aacparse"),
           "h264 rtmp keeps the AAC audio appsrc");
    expect(parses(tmpl), "h264 rtmp transcode template parses");
  }

  // h265 transcode -> RTMP: Enhanced FLV (eflvmux), NOT legacy flvmux.
  {
    const std::string tmpl =
        output_template(output_proto::rtmp, h265, "vah265enc");
    expect(has(tmpl, "vah265enc name=venc"), "h265 rtmp encoder");
    expect(has(tmpl, "h265parse config-interval=-1"), "h265 rtmp parser");
    expect(has(tmpl, "video/x-h265,stream-format=hvc1,alignment=au"),
           "h265 rtmp hvc1 caps");
    expect(has(tmpl, "eflvmux name=mux") && !has(tmpl, " flvmux name=mux"),
           "h265 rtmp uses eflvmux, never legacy flvmux");
    expect(parses(tmpl), "h265 eflvmux transcode template parses");
  }

  // h265 transcode -> SRT: re-encoded video to mpegtsmux (video-only).
  {
    const std::string tmpl =
        output_template(output_proto::srt, h265, "vah265enc");
    expect(has(tmpl, "mpegtsmux alignment=7"), "srt mpegtsmux");
    expect(has(tmpl, "srtsink name=osink"), "srt sink");
    expect(!has(tmpl, "appsrc name=asrc"), "srt transcode is video-only");
    expect(parses(tmpl), "srt transcode template parses");
  }

  // h264 transcode -> RIST: substring only (see the file header note).
  {
    const std::string tmpl =
        output_template(output_proto::rist, h264, "x264enc");
    expect(has(tmpl, "x264enc name=venc"), "rist encoder");
    expect(has(tmpl, "h264parse config-interval=-1"), "rist parser");
    expect(has(tmpl, "mpegtsmux alignment=7") && has(tmpl, "ristsink name=osink"),
           "rist transcode chain");
  }

  // Decoder stage templates.
  {
    const std::string av1 = decoder_template(codec::av1, "av1parse", "vaav1dec");
    expect(has(av1, "av1parse") && has(av1, "vaav1dec"),
           "av1 decoder elements");
    expect(has(av1, "video/x-raw,format=NV12")
               && has(av1, "appsink name=dsink sync=false"),
           "decoder NV12 appsink");
    expect(parses(av1), "av1 decoder template parses");

    const std::string h264d =
        decoder_template(codec::h264, "h264parse", "vah264dec");
    expect(has(h264d, "h264parse ! vah264dec"), "h264 decoder chain");
    expect(parses(h264d), "h264 decoder template parses");
  }

  std::puts("pipeline_build_test: ALL OK");
  return 0;
}
