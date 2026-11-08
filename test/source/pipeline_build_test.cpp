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

#include <chrono>
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

  // Copy-only templates keep legacy flvmux and no encoder. The demuxer's pads
  // are dispatched by caps in demux_pad_added, so the template must not leave
  // pad selection to a `d.` link.
  {
    const std::string rtmp =
        output_template(output_proto::rtmp, transcode_config {}, nullptr);
    expect(has(rtmp, "flvmux name=mux") && !has(rtmp, "name=venc"),
           "copy rtmp keeps flvmux and has no encoder");
    // Nothing links the demuxer's pads in the template: a template link joins
    // whichever pad tsdemux announces first, and the pad-to-parser decision is
    // made from each pad's caps in demux_pad_added.
    expect(!has(rtmp, "d. !") && !has(rtmp, "tsdemux name=d !"),
           "copy rtmp links no demuxer pad from the template");
    expect(has(rtmp, "queue name=vq ! h264parse config-interval=-1"),
           "copy rtmp names the video branch");
    expect(has(rtmp, "queue name=aq ! aacparse name=aparse"),
           "copy rtmp names the audio branch");
    expect(!has(rtmp, "avdec_aac"),
           "copy rtmp carries AAC through instead of re-encoding it");
    expect(has(rtmp, "funnel name=ddrop ! fakesink sync=false async=false"),
           "copy rtmp has a drop funnel for pads no branch handles");
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
    // The branch converts the source into the declared format instead of
    // asserting it: an ingest at any rate must negotiate.
    expect(has(tmpl, "avdec_aac") && has(tmpl, "audioresample")
               && has(tmpl, "audio/x-raw,rate=48000,channels=2")
               && has(tmpl, "avenc_aac"),
           "h264 rtmp resamples the source audio into 48 kHz stereo");
    // The audio branch stops at the capsfilter: the muxer's audio pad is
    // requested from demux_pad_added() only, so a video-only source never
    // hands flvmux an audio pad to wait on and video publishes immediately.
    expect(has(tmpl, "capsfilter name=acaps "
                     "caps=audio/mpeg,mpegversion=4,stream-format=raw"),
           "h264 rtmp audio branch is a named capsfilter on the container only");
    expect(has(tmpl, "stream-format=raw ") && !has(tmpl, "codec_data="),
           "h264 rtmp states no AudioSpecificConfig of its own");
    expect(tmpl.find("stream-format=raw ! mux.") == std::string::npos,
           "h264 rtmp audio branch is not pre-linked to the muxer");
    // The encoder input is pinned: the destinations are 4:2:0 8-bit, and the
    // encoder's own sink caps would otherwise accept 4:4:4 or 10-bit layouts.
    expect(has(tmpl, "video/x-raw,format=NV12 ! vah264enc name=venc"),
           "h264 rtmp encoder input is pinned to NV12 (4:2:0, 8-bit)");
    // A video-only ingest makes the audio branch's tsdemux expose a video pad;
    // left unlinked it fails the pipeline with "streaming stopped, reason=
    // not-linked". The funnel is where that pad goes.
    expect(has(tmpl, "funnel name=ddrop ! fakesink sync=false async=false"),
           "h264 rtmp has a drop funnel for the audio branch's non-audio pads");
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
    expect(has(tmpl, "video/x-raw,format=NV12 ! vah265enc name=venc"),
           "srt encoder input is pinned to NV12 (4:2:0, 8-bit)");
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
    expect(has(av1, "block=false") && !has(av1, "block=true"),
           "decode appsrc never blocks the feed thread");
    // The drop sink must never be able to hold the pipeline in preroll: a sink
    // that has no data yet keeps the bin ASYNC forever with no error, which is
    // exactly how the decode stage produced zero frames.
    expect(has(av1, "fakesink sync=false async=false"),
           "decode drop sink does not gate preroll");
    expect(parses(av1), "av1 decoder template parses");

    // The parser's output is pinned to the decoder's own sink caps: h264parse
    // must produce the access-unit alignment vah264dec declares.
    const std::string h264d =
        decoder_template(codec::h264, "h264parse", "vah264dec");
    expect(has(h264d, "h264parse ! video/x-h264,alignment=au ! vah264dec"),
           "h264 decoder chain pins the decoder's alignment");
    // A decoder that declares no single-value field gets no caps filter at all.
    expect(!has(h264d, "profile="),
           "no profile is pinned where the decoder lists several");
    expect(parses(h264d), "h264 decoder template parses");
  }

  // Transit-buffer watchdog phases (output::wait_appsrc_drain). A connect
  // attempt that has not published holds its first buffer through the RTMP
  // handshake and publish exchange (6-7 s against a local sink), so the stall
  // window must not be applied to it: judging a rebuild by that window tears the
  // pipeline down before rtmp2sink can accept the publish.
  {
    using std::chrono::milliseconds;
    constexpr bool establishing = false;
    constexpr bool publishing = true;

    expect(classify_drain_hold(milliseconds {6000}, establishing)
               == drain_verdict::keep_waiting,
           "an establishing output held for 6 s is not stalled");
    expect(classify_drain_hold(output_timing::connect_timeout
                                   - milliseconds {1000},
                               establishing)
               == drain_verdict::keep_waiting,
           "an establishing output is not stalled anywhere inside the connect window");
    expect(classify_drain_hold(output_timing::connect_timeout
                                   + milliseconds {1000},
                               establishing)
               == drain_verdict::publish_not_established,
           "a connect window that closes without a publish rebuilds the output");
    expect(classify_drain_hold(output_timing::stall_timeout, establishing)
               == drain_verdict::keep_waiting,
           "the mid-stream stall window does not apply to an establishing output");
    expect(output_timing::connect_timeout > output_timing::stall_timeout,
           "the connect budget exceeds the stall budget");

    // ... while a sink that was publishing is still judged on the stall window.
    expect(classify_drain_hold(milliseconds {2000}, publishing)
               == drain_verdict::keep_waiting,
           "a publishing sink inside the stall window keeps waiting");
    expect(classify_drain_hold(milliseconds {6000}, publishing)
               == drain_verdict::stalled,
           "a publishing sink pinned past the stall window is stalled");
    expect(classify_drain_hold(output_timing::connect_timeout
                                   + milliseconds {1000},
                               publishing)
               == drain_verdict::stalled,
           "a publishing sink is judged by the stall window, not the connect one");
  }

  std::puts("pipeline_build_test: ALL OK");
  return 0;
}
