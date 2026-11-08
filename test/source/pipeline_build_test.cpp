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

  // -------------------------------------------------------------------------
  // Standby-enabled (ES-fed) shapes. With standby_config.enabled the stage
  // demuxes once and publishes access units, so a copy output remuxes AUs and
  // must contain no demuxer of its own; the disabled path must stay byte for
  // byte what it was.
  // -------------------------------------------------------------------------
  {
    const es_input_spec off {};
    for (const output_proto proto :
         {output_proto::rtmp, output_proto::rtmps, output_proto::srt,
          output_proto::rist}) {
      for (const transcode_config& cfg : {transcode_config {}, h264, h265}) {
        const char* enc = cfg.target == transcode_target::h265 ? "vah265enc"
            : cfg.target == transcode_target::h264                 ? "vah264enc"
                                                                  : nullptr;
        expect(output_template(proto, cfg, enc, off)
                   == output_template(proto, cfg, enc),
               "the disabled path is exactly the pre-standby template");
      }
    }

    es_input_spec es;
    es.enabled = true;
    es.video_codec = codec::h264;
    es.audio = true;

    const std::string rtmp = output_template(output_proto::rtmp,
                                             transcode_config {}, nullptr, es);
    std::printf("es copy rtmp: %s\n", rtmp.c_str());
    expect(!has(rtmp, "tsparse") && !has(rtmp, "tsdemux"),
           "es copy rtmp has no demuxer of its own");
    expect(has(rtmp, "h264parse config-interval=-1"),
           "es copy rtmp parses the AUs with config-interval=-1");
    expect(has(rtmp, "video/x-h264,stream-format=avc,alignment=au"),
           "es copy rtmp asks the parser for AVC access units");
    expect(has(rtmp, "flvmux name=mux"), "es copy rtmp keeps flvmux");
    expect(has(rtmp, "appsrc name=asrc") && has(rtmp, "aacparse name=aparse"),
           "es copy rtmp takes audio AUs from the audio es_ring");
    expect(has(rtmp, "funnel name=ddrop ! fakesink sync=false async=false"),
           "es copy rtmp keeps the drop funnel");
    expect(parses(rtmp), "es copy rtmp template parses");

    // Video-only: no audio branch at all, so flvmux never waits on it.
    es_input_spec es_video_only = es;
    es_video_only.audio = false;
    const std::string rtmp_vo = output_template(output_proto::rtmp,
                                                transcode_config {},
                                                nullptr, es_video_only);
    expect(!has(rtmp_vo, "appsrc name=asrc") && !has(rtmp_vo, "aacparse"),
           "es copy rtmp video-only has no audio appsrc");
    expect(parses(rtmp_vo), "es copy rtmp video-only template parses");

    // HEVC copy on rtmp must use Enhanced FLV, like the transcode tier.
    es_input_spec es_hevc = es;
    es_hevc.video_codec = codec::h265;
    const std::string rtmp_hevc = output_template(
        output_proto::rtmp, transcode_config {}, nullptr, es_hevc);
    expect(has(rtmp_hevc, "h265parse config-interval=-1")
               && has(rtmp_hevc, "video/x-h265,stream-format=hvc1,alignment=au")
               && has(rtmp_hevc, "eflvmux name=mux")
               && !has(rtmp_hevc, " flvmux name=mux"),
           "es copy rtmp h265 uses eflvmux and hvc1 caps");
    expect(parses(rtmp_hevc), "es copy rtmp h265 template parses");

    const std::string srt =
        output_template(output_proto::srt, transcode_config {}, nullptr, es);
    std::printf("es copy srt: %s\n", srt.c_str());
    expect(!has(srt, "tsparse") && !has(srt, "tsdemux"),
           "es copy srt has no demuxer of its own");
    expect(has(srt, "h264parse config-interval=-1"),
           "es copy srt parses the AUs with config-interval=-1");
    expect(has(srt, "mpegtsmux name=mux alignment=7")
               && has(srt, "srtsink name=osink"),
           "es copy srt remuxes the AUs into its own TS");
    expect(parses(srt), "es copy srt template parses");

    const std::string rist =
        output_template(output_proto::rist, transcode_config {}, nullptr, es);
    std::printf("es copy rist: %s\n", rist.c_str());
    expect(!has(rist, "tsparse") && !has(rist, "tsdemux"),
           "es copy rist has no demuxer of its own");
    expect(has(rist, "mpegtsmux name=mux alignment=7")
               && has(rist, "ristsink name=osink"),
           "es copy rist remuxes the AUs into its own TS");

    // Transcode rtmp: video still from the frame ring, audio from the es_ring.
    const std::string tr =
        output_template(output_proto::rtmp, h264, "vah264enc", es);
    std::printf("es transcode rtmp: %s\n", tr.c_str());
    expect(!has(tr, "tsparse") && !has(tr, "tsdemux"),
           "es transcode rtmp runs no per-output demuxer");
    expect(has(tr, "vah264enc name=venc") && has(tr, "flvmux name=mux"),
           "es transcode rtmp keeps the encode chain and muxer");
    expect(has(tr, "appsrc name=asrc") && has(tr, "avdec_aac"),
           "es transcode rtmp re-encodes the audio AUs");
    expect(parses(tr), "es transcode rtmp template parses");

    // A transcode srt/rist output is video-only either way: the ES flag must
    // not change its string.
    expect(output_template(output_proto::srt, h265, "vah265enc", es)
               == output_template(output_proto::srt, h265, "vah265enc"),
           "es transcode srt is unchanged (video-only)");
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

    // ES input mode (standby enabled): the stage's one demux already ran, so
    // the decode stage must not demux again.
    const std::string esd =
        decoder_es_template(codec::h264, "h264parse", "vah264dec");
    std::printf("es decoder: %s\n", esd.c_str());
    expect(!has(esd, "tsparse") && !has(esd, "tsdemux"),
           "the es decoder template demuxes nothing");
    expect(has(esd, "caps=video/x-h264") && has(esd, "h264parse"),
           "the es decoder declares the AU media type and parses it");
    expect(has(esd, "queue name=vqueue")
               && has(esd, "video/x-raw,format=NV12")
               && has(esd, "appsink name=dsink sync=false"),
           "the es decoder keeps the frame-ring hand-off unchanged");
    expect(has(esd, "block=false") && !has(esd, "block=true"),
           "the es decode appsrc never blocks the feed thread");
    expect(parses(esd), "es decoder template parses");

    const std::string esd_av1 =
        decoder_es_template(codec::av1, "av1parse", "av1dec");
    expect(has(esd_av1, "caps=video/av1"), "es av1 decoder declares video/av1");
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
