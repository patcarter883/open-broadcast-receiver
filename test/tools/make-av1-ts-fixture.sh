#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# make-av1-ts-fixture.sh — produce an AV1-in-MPEG-TS fixture that this
# receiver's decode chain (filesrc -> tsparse -> tsdemux -> av1parse ->
# vaav1dec -> videoconvert -> NV12) actually accepts.
#
# WHY THE MUXER FLAGS MATTER (this is the whole point of the script)
# -----------------------------------------------------------------
# MPEG-TS has no standardised stream type for AV1, so mpegtsmux refuses it by
# default with "Failed to determine stream type or mapping is not supported".
# The product's encoder does not work around that by post-processing the TS: it
# sets `enable-custom-mappings=true` on mpegtsmux, which makes the muxer emit
# the custom stream type it defines for AV1 (0x06 + AV1 registration). The
# receiver therefore only ever sees AV1-in-TS that was muxed with that flag, and
# a fixture muxed without it is not representative -- tsdemux rejects it with
# "This stream contains no valid or supported streams", which is exactly how a
# previous investigation was misled.
#
# The flag and the surrounding settings are copied from the encoder repo
# (open-broadcast-encoder v2, source/encode/encode.cpp, pipeline_build_sink):
#
#   mpegtsmux alignment=7 enable-custom-mappings=true       <- the required pair
#   vaav1enc bitrate=<> rate-control=cbr key-int-max=60      <- the AV1 encoder
#   av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame
#
# `alignment=7` is 7 TS packets per buffer = 1316 bytes = the RIST datagram the
# receiver feeds its ts_ring in. av1parse is not optional: encoders emit
# alignment=tu and mpegtsmux only accepts alignment=frame.
#
# USAGE
#   test/tools/make-av1-ts-fixture.sh [-n|--no-verify] <out.ts>
#
#   FIXTURE_ENCODER=av1enc   force the software encoder (also: vaav1enc)
#   FIXTURE_FRAMES=150       frame count (default 150 = 5 s at 30 fps)
#
# Exit status: 0 = fixture written (and, unless --no-verify, proven decodable by
# tsdemux); non-zero with a diagnostic otherwise. One command, no arguments
# beyond the output path:
#
#   ./test/tools/make-av1-ts-fixture.sh build/av1-fixture.ts
#
set -eu

usage()
{
  echo "usage: $0 [-n|--no-verify] <out.ts>" >&2
  exit 2
}

verify=1
while [ $# -gt 0 ]; do
  case "$1" in
    -n|--no-verify) verify=0; shift ;;
    -h|--help) usage ;;
    -*) echo "$0: unknown option $1" >&2; usage ;;
    *) break ;;
  esac
done
[ $# -eq 1 ] || usage
out=$1
mkdir -p "$(dirname -- "$out")"

command -v gst-launch-1.0 >/dev/null 2>&1 || {
  echo "$0: gst-launch-1.0 not found (install gstreamer1.0-tools)" >&2
  exit 1
}

# mpegtsmux must have enable-custom-mappings, which arrived with the 1.26/1.28
# series; without it the script cannot produce a representative fixture at all,
# so fail here rather than emit a TS the receiver can never demux.
if ! gst-inspect-1.0 mpegtsmux 2>/dev/null | grep -q enable-custom-mappings; then
  echo "$0: this mpegtsmux has no enable-custom-mappings;" >&2
  echo "    AV1-in-MPEG-TS cannot be muxed. Need GStreamer >= 1.28." >&2
  exit 1
fi

# Encoder: the deployment's VAAPI AV1 encoder first (that is what the encoder
# repo runs on this hardware), then aomenc. `videoconvert` is unconditional:
# vaav1enc only takes DMABuf/NV12-class input, not the I420 testsrc produces.
pick_encoder()
{
  if [ -n "${FIXTURE_ENCODER:-}" ]; then
    if ! gst-inspect-1.0 "$FIXTURE_ENCODER" >/dev/null 2>&1; then
      echo "$0: FIXTURE_ENCODER=$FIXTURE_ENCODER is not installed" >&2
      exit 1
    fi
    echo "$FIXTURE_ENCODER"
    return
  fi
  for cand in vaav1enc av1enc; do
    if gst-inspect-1.0 "$cand" >/dev/null 2>&1; then
      echo "$cand"
      return
    fi
  done
  echo "$0: no AV1 encoder (vaav1enc / av1enc) installed" >&2
  exit 1
}

encoder=$(pick_encoder)
frames=${FIXTURE_FRAMES:-150}

# Encoder-side settings per element. Both are CBR 4 Mbit/s with a 60-frame
# keyframe interval, matching the encoder repo's AV1 chain closely enough that
# the receiver sees the same SH/sequence-header cadence a live stream has.
case "$encoder" in
  vaav1enc) enc_args="bitrate=4000 rate-control=cbr key-int-max=60" ;;
  av1enc)   enc_args="target-bitrate=4000 cpu-used=8 lag-in-frames=0 row-mt=true tile-columns=2 keyframe-max-dist=60" ;;
  *)        enc_args="bitrate=4000" ;;
esac

echo "fixture: encoder=$encoder frames=$frames -> $out"

rm -f "$out"
# -e: send EOS so the muxer flushes the final PSI/PES before the filesink closes.
if ! gst-launch-1.0 -e -q \
    videotestsrc num-buffers="$frames" pattern=smpte is-live=false \
    ! video/x-raw,format=I420,width=1280,height=720,framerate=30/1 \
    ! videoconvert \
    ! "$encoder" $enc_args \
    ! av1parse \
    ! video/x-av1,stream-format=obu-stream,alignment=frame \
    ! mpegtsmux alignment=7 enable-custom-mappings=true \
    ! filesink location="$out"
then
  echo "$0: muxing failed" >&2
  exit 1
fi

size=$(wc -c < "$out")
echo "fixture: wrote $size bytes"
[ "$size" -gt 0 ] || { echo "$0: empty fixture" >&2; exit 1; }

if [ "$verify" -eq 1 ]; then
  # The check a fabricated TS fails: tsdemux must expose a video/x-av1 stream
  # and av1parse must accept it. `num-buffers=1` bounds it; a non-zero exit or
  # an "erroneous pipeline"/"no valid or supported streams" log is a failure.
  # The status, not a grep, decides -- an error pattern list always misses one.
  log=$(mktemp "${TMPDIR:-/tmp}/av1-fixture-verify.XXXXXX")
  trap 'rm -f "$log"' EXIT
  if ! gst-launch-1.0 -e -q \
      filesrc location="$out" \
      ! tsparse \
      ! tsdemux \
      ! av1parse \
      ! fakesink sync=false num-buffers=1 >"$log" 2>&1
  then
    echo "$0: tsdemux/av1parse rejected the fixture:" >&2
    sed -n '1,20p' "$log" >&2
    exit 1
  fi
  if grep -qiE "no valid or supported streams|Failed to determine stream type|erroneous pipeline|not-linked" "$log"; then
    echo "$0: tsdemux/av1parse rejected the fixture:" >&2
    sed -n '1,20p' "$log" >&2
    exit 1
  fi
  echo "fixture: tsdemux exposes an AV1 elementary stream (verified)"
fi
