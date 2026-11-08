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
#   FIXTURE_BITRATE=4000     CBR bitrate in kbit/s (default 4000)
#   FIXTURE_AUDIO=0          omit the AAC track (default: AAC-LC 48 kHz stereo,
#                            ADTS-framed, the source the RTMP fixup is built for)
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
bitrate=${FIXTURE_BITRATE:-4000}

# Encoder-side settings per element. CBR at FIXTURE_BITRATE (default 4 Mbit/s)
# with a 60-frame keyframe interval, matching the encoder repo's AV1 chain
# closely enough that the receiver sees the same SH/sequence-header cadence a
# live stream has.
case "$encoder" in
  vaav1enc) enc_args="bitrate=$bitrate rate-control=cbr key-int-max=60" ;;
  av1enc)   enc_args="target-bitrate=$bitrate cpu-used=8 lag-in-frames=0 row-mt=true tile-columns=2 keyframe-max-dist=60" ;;
  *)        enc_args="bitrate=$bitrate" ;;
esac

echo "fixture: encoder=$encoder frames=$frames -> $out"

rm -f "$out"
# The mux is named and both branches are attached with request pads (`m.`),
# because a mux that is itself the tail of the video chain has no second request
# pad and the audio branch then fails with "not-linked".
#
# AUDIO is AAC-LC 48 kHz stereo, ADTS-framed: the source the receiver is built
# for (source/fanout/output.cpp hardcodes `stream-format=raw, rate=48000,
# channels=2, codec_data=1190` for flvmux). mpegtsmux only takes mpegversion=4
# with `framed=true`, so aacparse's output is pinned to that. One AAC buffer is
# 1024 samples, so it is sized to the same wall time as the video.
duration_ms=$(( frames * 1000 / 30 ))
audio_buffers=$(( duration_ms * 48000 / (1024 * 1000) ))
[ "$audio_buffers" -gt 0 ] || audio_buffers=1
# The chain is one token string: gst-parse starts a new chain at `m.`, and the
# audio statement is present only when audio is enabled, so FIXTURE_AUDIO=0
# produces a single-branch video-only TS.
chain="mpegtsmux alignment=7 enable-custom-mappings=true name=m
  videotestsrc num-buffers=$frames pattern=smpte is-live=false
  ! video/x-raw,format=I420,width=${FIXTURE_WIDTH:-1280},height=${FIXTURE_HEIGHT:-720},framerate=30/1
  ! videoconvert
  ! $encoder $enc_args
  ! av1parse
  ! video/x-av1,stream-format=obu-stream,alignment=frame
  ! m."
if [ "${FIXTURE_AUDIO:-1}" != "0" ]; then
  chain="$chain
  audiotestsrc num-buffers=$audio_buffers wave=sine freq=440 is-live=false
  ! audio/x-raw,rate=48000,channels=2
  ! avenc_aac bitrate=128000
  ! aacparse
  ! audio/mpeg,mpegversion=4,stream-format=adts,framed=true
  ! m."
fi
chain="$chain
  m. ! filesink location=$out"

# unquoted on purpose: the chain is a token stream for gst-launch, split on
# whitespace. Every token is composed above from values without spaces.
# shellcheck disable=SC2086
# -e: send EOS so the muxer flushes the final PSI/PES before the filesink closes.
if ! gst-launch-1.0 -e -q $chain
then
  echo "$0: muxing failed" >&2
  exit 1
fi

size=$(wc -c < "$out")
echo "fixture: wrote $size bytes"
[ "$size" -gt 0 ] || { echo "$0: empty fixture" >&2; exit 1; }

# Sidecar so a later consumer (test/harness/receiver_e2e_local.sh) feeds the
# file at its real frame rate instead of assuming a default frame count.
printf 'encoder=%s\nframes=%s\nfps=30\nwidth=%s\nheight=%s\nbytes=%s\n' \
  "$encoder" "$frames" "${FIXTURE_WIDTH:-1280}" "${FIXTURE_HEIGHT:-720}" "$size" \
  > "$out.meta"

if [ "$verify" -eq 1 ]; then
  # The check a fabricated TS fails: tsdemux must expose a video/x-av1 stream
  # and av1parse must accept it, and -- when the fixture carries audio -- an
  # audio/mpeg stream that aacparse accepts. `num-buffers=1` bounds it; a
  # non-zero exit or an "erroneous pipeline"/"no valid or supported streams" log
  # is a failure. The status, not a grep, decides -- an error pattern list
  # always misses one.
  log=$(mktemp "${TMPDIR:-/tmp}/av1-fixture-verify.XXXXXX")
  trap 'rm -f "$log"' EXIT
  vchain="filesrc location=$out
    ! tsparse
    ! tsdemux name=d
    d. ! queue ! av1parse ! fakesink sync=false num-buffers=1"
  if [ "${FIXTURE_AUDIO:-1}" != "0" ]; then
    vchain="$vchain
    d. ! queue ! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts,framed=true
    ! fakesink sync=false num-buffers=1"
  fi
  # shellcheck disable=SC2086
  if ! gst-launch-1.0 -e -q $vchain >"$log" 2>&1
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
  if [ "${FIXTURE_AUDIO:-1}" != "0" ]; then
    echo "fixture: tsdemux exposes an AV1 elementary stream and an AAC one (verified)"
  else
    echo "fixture: tsdemux exposes an AV1 elementary stream (verified)"
  fi
fi
