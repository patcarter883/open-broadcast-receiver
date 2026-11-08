#!/usr/bin/env bash
# Decode-stage regression: generate a tiny AV1-in-MPEG-TS fixture and run the
# receiver's real decode stage against it.
#
#   run_decode_fixture.sh <decode_file-binary> <fixture.ts>
#
# The fixture is generated once per build directory (30 frames of 320x180 at
# 30 fps, ~1 s) and reused. Exit 77 (CTest SKIP_RETURN_CODE) with a printed
# reason when the AV1 encoder, the VA-API AV1 decoder, or a render node is
# absent, so the test skips instead of failing on a box without them.
set -euo pipefail

binary="${1:?usage: run_decode_fixture.sh <decode_file> <fixture.ts>}"
fixture="${2:?usage: run_decode_fixture.sh <decode_file> <fixture.ts>}"

frames=30
width=320
height=180
fps=30
# The stage is fed over the fixture's own duration; one extra second covers the
# muxer's initial timestamp offset.
duration=2

skip() {
    printf 'SKIP: %s\n' "$*"
    exit 77
}

for tool in gst-launch-1.0 gst-inspect-1.0; do
    command -v "$tool" >/dev/null 2>&1 || skip "$tool not on PATH"
done

for element in av1enc av1parse vaav1dec mpegtsmux tsdemux; do
    gst-inspect-1.0 "$element" >/dev/null 2>&1 \
        || skip "GStreamer element $element is not installed"
done

# vaav1dec needs a VA-API render node; there is no software AV1 decoder in the
# element list the receiver accepts.
[ -d /dev/dri ] && ls /dev/dri/renderD* >/dev/null 2>&1 \
    || skip "no /dev/dri render node for vaav1dec"

if [ ! -s "$fixture" ]; then
    printf 'generating fixture %s\n' "$fixture"
    gst-launch-1.0 -q -e videotestsrc num-buffers="$frames" \
        ! "video/x-raw,format=I420,width=$width,height=$height,framerate=$fps/1" \
        ! av1enc cpu-used=8 lag-in-frames=0 row-mt=true tile-columns=2 \
          target-bitrate=4000 keyframe-max-dist=30 \
        ! av1parse ! "video/x-av1,stream-format=obu-stream,alignment=frame" \
        ! mpegtsmux alignment=7 enable-custom-mappings=true \
        ! filesink location="$fixture" >/dev/null 2>&1 \
        || skip "av1enc could not produce a fixture"
fi
[ -s "$fixture" ] || skip "fixture $fixture is empty"

printf 'fixture %s: %s bytes\n' "$fixture" "$(stat -c %s "$fixture")"
exec "$binary" "$fixture" av1 --expect-frames "$frames" --duration "$duration"
