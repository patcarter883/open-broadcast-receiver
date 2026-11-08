#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# rist_av1_publish_e2e.sh — prove the whole publish path on one box, with no
# capture rig and no real destination:
#
#   synthetic AV1+AAC MPEG-TS  ->  RIST  ->  receiver  ->  vaav1dec
#     -> frame ring -> vah264enc -> flvmux -> RTMP -> local server
#
# Every stage is the product's own: the TS is muxed the way the encoder muxes
# it (mpegtsmux enable-custom-mappings=true, AV1 carried as stream_type 0x06 /
# registration AV1G), it is sent by test/tools/rist_feed with the ADVANCED
# profile the receiver listens with, and the destination is a throwaway local
# mediamtx whose record directory is the measurement. Nothing leaves the box:
# the receiver refuses loopback destinations by policy, so the destination is
# this host's LAN address and --egress-allow-private is what the operator would
# use to restream into a LAN.
#
# PASS requires, all measured:
#   * the output never reconnected during the run,
#   * the receiver's bytes_published_total grew,
#   * mediamtx logged "is publishing to path '<app>/<key>'",
#   * mediamtx wrote >= 256 KiB of recordable media (a handshake alone is ~3 KiB,
#     so "greater than zero" would pass a connect-and-send-nothing publish).
#
# Usage: test/harness/rist_av1_publish_e2e.sh [seconds]
#   Env: RECEIVER (default build/open-broadcast-receiver)
#        WORK (default build/e2e)
# Exit: 0 pass, 1 fail, 77 mediamtx or an element is missing.

set -euo pipefail

seconds="${1:-30}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
receiver="${RECEIVER:-$root/build/open-broadcast-receiver}"
feed="$root/build/test/rist_feed"
work="${WORK:-$root/build/e2e}"
fixture="$work/av1-aac-fixture.ts"
rec_dir="$work/rec"
# These are overridable because a fixed 5000/UDP collides with anything else on
# the box that listens there (a rist2rist proxy, another harness). Without an
# override the run "passes" the fixture check, ships its packets to the other
# socket, and fails with bytes_published_total=0 and no explanation.
control_port="${CONTROL_PORT:-18081}"
rtmp_port="${RTMP_PORT:-1935}"
api_port="${API_PORT:-9997}"
rist_port="${RIST_PORT:-5000}"

for bin in "$receiver" "$feed" mediamtx gst-launch-1.0 pv ffprobe curl jq; do
  if ! command -v "$bin" >/dev/null 2>&1 && [ ! -x "$bin" ]; then
    echo "SKIP: missing $bin" >&2
    exit 77
  fi
done
for element in vaav1enc vaav1dec vah264enc av1parse flvmux rtmp2sink mpegtsmux tsdemux; do
  gst-inspect-1.0 "$element" >/dev/null 2>&1 || { echo "SKIP: missing element $element" >&2; exit 77; }
done

mkdir -p "$work"
lan_ip="$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)"
[ -n "$lan_ip" ] || { echo "SKIP: no non-loopback IPv4 address" >&2; exit 77; }

# A crash here is worth 3 wasted minutes: the receiver's listener binds happily
# next to another owner of the same UDP port and the packets go to the other
# socket, so the run reports a healthy RIST handshake and zero decoded frames.
if holder="$(ss -lunpH "sport = :$rist_port" 2>/dev/null | head -1)" && [ -n "$holder" ]; then
  echo "FAIL: udp/$rist_port is already held by $holder" >&2
  echo "      that socket will take the feed; rerun with RIST_PORT=<free port>" >&2
  exit 2
fi

cleanup() {
  [ -n "${receiver_pid:-}" ] && kill "$receiver_pid" 2>/dev/null || true
  [ -n "${sink_pid:-}" ] && kill "$sink_pid" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

# ---- 1) synthetic AV1+AAC MPEG-TS, muxed with the encoder's settings --------
if [ ! -s "$fixture" ]; then
  echo "building $fixture (vaav1enc + avenc_aac -> mpegtsmux custom mappings)"
  gst-launch-1.0 -e -q \
    videotestsrc num-buffers=$((seconds * 30)) pattern=smpte is-live=false \
      ! video/x-raw,format=I420,width=1280,height=720,framerate=30/1 \
      ! videoconvert ! vaav1enc bitrate=4000 rate-control=cbr key-int-max=60 \
      ! av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame ! mux. \
    audiotestsrc num-buffers=$((seconds * 47)) wave=sine is-live=false \
      ! audio/x-raw,rate=48000,channels=2 ! audioconvert ! audioresample \
      ! avenc_aac bitrate=128000 ! aacparse \
      ! audio/mpeg,mpegversion=4,stream-format=raw ! mux. \
    mpegtsmux name=mux alignment=7 enable-custom-mappings=true \
      ! filesink location="$fixture"
fi

# The fixture is only usable if the receiver's own parser sees AV1 on it.
vpad="$(gst-launch-1.0 -v filesrc location="$fixture" ! tsparse ! tsdemux ! fakesink num-buffers=1 2>&1 \
        | grep -m1 'caps = video/x-av1' || true)"
if [ -z "$vpad" ]; then
  echo "FAIL: tsdemux exposes no video/x-av1 pad on $fixture" >&2
  exit 1
fi
echo "fixture check: $vpad"

# ---- 2) throwaway local RTMP server ----------------------------------------
cat > "$work/mediamtx.yml" <<YAML
logLevel: info
logDestinations: [stdout]
api: yes
apiAddress: 127.0.0.1:$api_port
rtsp: no
rtmp: yes
rtmpAddress: :$rtmp_port
hls: no
webrtc: no
srt: no
paths:
  all_others:
    record: yes
    recordPath: $rec_dir/%path/%Y%m%d-%H%M%S
    recordFormat: fmp4
    recordSegmentDuration: 2s
YAML
rm -rf "$rec_dir/live/e2e"
# Run mediamtx from the harness directory: it writes its MoQ certificate
# (auto.crt/auto.key) into the working directory.
( cd "$work" && exec mediamtx mediamtx.yml ) > "$work/mediamtx.log" 2>&1 &
sink_pid=$!
sleep 1
curl -sf "http://127.0.0.1:$api_port/v3/config/global/get" >/dev/null \
  || { echo "FAIL: mediamtx API did not come up" >&2; exit 1; }

# ---- 3) the receiver, ingesting over RIST and publishing to the sink -------
"$receiver" --control-port "$control_port" --rist-port "$rist_port" --egress-allow-private \
  > "$work/receiver.log" 2>&1 &
receiver_pid=$!
sleep 1

cat > "$work/start.json" <<JSON
{
  "schema_version": 4,
  "session_id": "e2e-av1-rtmp",
  "ingest": { "rist_listen": "rist://@127.0.0.1:$rist_port", "bandwidth": 8000 },
  "source": { "codec": "av1" },
  "outputs": [
    {
      "id": "local-rtmp",
      "type": "rtmp",
      "url": "rtmp://$lan_ip:$rtmp_port/live",
      "key_or_streamid": "e2e",
      "transcode": { "codec": "h264", "bitrate_kbps": 2500, "gop": 30 }
    }
  ]
}
JSON
curl -sf -X POST --data-binary @"$work/start.json" \
  "http://127.0.0.1:$control_port/start" > /dev/null \
  || { echo "FAIL: /start refused:"; tail -5 "$work/receiver.log"; exit 1; }
sleep 1

# ---- 4) feed the fixture at its own bitrate --------------------------------
bytes_per_s=$(( $(stat -c %s "$fixture") / seconds + 1 ))
(
  pv -q -L "${bytes_per_s}" "$fixture" | "$feed" 127.0.0.1 "$rist_port" >/dev/null 2>&1
) &
feed_pid=$!

printf '%-4s %-22s %-12s %-6s %-9s %s\n' t bytes_published_total state recon dropped recorded_bytes
first_bytes=0
last_bytes=0
publishing=0
for ((t = 2; t <= seconds + 6; t += 2)); do
  stats="$(curl -s "http://127.0.0.1:$control_port/stats")"
  sent="$(jq -r '.outputs[0].bytes_published_total // 0' <<<"$stats")"
  state="$(jq -r '.outputs[0].state // "?"' <<<"$stats")"
  recon="$(jq -r '.outputs[0].reconnects // 0' <<<"$stats")"
  dropped="$(jq -r '.outputs[0].transcode.frames_dropped // 0' <<<"$stats")"
  rec="$(du -sb "$rec_dir/live/e2e" 2>/dev/null | cut -f1 || true)"
  rec="${rec:-0}"
  [ "$first_bytes" -eq 0 ] && first_bytes="$sent"
  last_bytes="$sent"
  grep -q "is publishing to path 'live/e2e'" "$work/mediamtx.log" && publishing=1
  printf '%-4s %-14s %-12s %-6s %-9s %s\n' "${t}s" "$sent" "$state" "$recon" "$dropped" "$rec"
  sleep 2
done
wait "$feed_pid" 2>/dev/null || true

recorded="$(du -sb "$rec_dir/live/e2e" 2>/dev/null | cut -f1 || true)"
recorded="${recorded:-0}"
reconnects="$(curl -s "http://127.0.0.1:$control_port/stats" | jq -r '.outputs[0].reconnects // 0')"

echo
echo "bytes_published_total: first=$first_bytes last=$last_bytes delta=$((last_bytes - first_bytes))"
echo "recorded:   $recorded bytes   publishing_logged=$publishing   reconnects=$reconnects"

fail=0
[ "$last_bytes" -gt "$first_bytes" ] || { echo "FAIL: bytes_published_total did not grow"; fail=1; }
[ "$publishing" -eq 1 ] || { echo "FAIL: the sink never reported a publish"; fail=1; }
[ "$recorded" -ge 262144 ] || { echo "FAIL: the sink recorded under 256 KiB ($recorded)"; fail=1; }
[ "$reconnects" -eq 0 ] || { echo "FAIL: the output reconnected $reconnects time(s)"; fail=1; }

# The sink's own file is the last word: it must parse as H.264 video.
seg="$(ls "$rec_dir/live/e2e"/*.mp4 2>/dev/null | tail -1 || true)"
if [ -n "$seg" ]; then
  echo "last segment: $(basename "$seg") $(ffprobe -v error -show_entries format=duration -of default=nk=1:nw=1 "$seg")s $(ffprobe -v error -show_entries stream=codec_name,width,height -of csv=p=0 "$seg" | head -1)"
else
  echo "FAIL: the sink wrote no segment"; fail=1
fi

[ "$fail" -eq 0 ] && echo "PASS" || echo "FAIL"
exit "$fail"
