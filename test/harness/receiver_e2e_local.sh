#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# receiver_e2e_local.sh — end-to-end exercise of the WHOLE receiver binary with
# no rig and no network dependency:
#
#   AV1-in-MPEG-TS fixture -> rist_feed (real RIST/ADVANCED ingest) ->
#   open-broadcast-receiver -> POST /start -> decode (vaav1dec) ->
#   transcode (vah264enc) -> flvmux -> rtmp2sink -> local MediaMTX
#
# Everything runs on this machine: the RIST sender, the receiver, and the RTMP
# sink. The only non-loopback hop is receiver -> sink, which is an address of
# this box's own interface because the receiver's egress policy always refuses
# loopback destinations (source/lib/lib.cpp classify(): 127.0.0.0/8 ->
# "loopback", even with --egress-allow-private). Set SINK_HOST to override.
#
# It judges the run the way an operator would, not by trusting the receiver's
# own counters:
#   * fixture validity  — ffprobe + a tsdemux parse-back that must expose a
#                         video/x-av1 pad (and the PMT bytes: stream_type 0x06
#                         with the AV1G registration descriptor and av1C).
#   * decode progress   — the decoder's own staged counters in its log lines
#                         (in chunks / demux / decoded / sink / state).
#   * publish           — the RTMP SERVER's byte counter for the path
#                         (MediaMTX /v3/paths/list bytesReceived), which cannot
#                         be inflated by the receiver. This matters: for a
#                         transcode output `bytes_sent` counts NV12 frames
#                         pushed into the video appsrc, so it grows even when
#                         nothing reaches RTMP.
#
# Usage:
#   test/harness/receiver_e2e_local.sh
#
# Env overrides: BIN FEED FIXTURE FIXTURE_FRAMES CTRL_PORT RIST_PORT RTMP_PORT
#                MTX_API_PORT SINK_HOST LOG_DIR KEEP=1 (leave processes up)
#
# Exit status: 0 = decode + publish both proven, 1 = a stage stopped (the log
# says which), 2 = the harness could not run (missing binary/element/port).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${BIN:-${REPO_ROOT}/build/open-broadcast-receiver}"
FEED="${FEED:-${REPO_ROOT}/build/test/rist_feed}"
BUILD_DIR="${REPO_ROOT}/build"
FIXTURE="${FIXTURE:-${BUILD_DIR}/fixtures/e2e-av1.ts}"
FIXTURE_FRAMES_GIVEN="${FIXTURE_FRAMES:+1}"
FIXTURE_FRAMES="${FIXTURE_FRAMES:-300}"        # 10 s at 30 fps
FIXTURE_FPS=30
CTRL_PORT="${CTRL_PORT:-18180}"
RIST_PORT="${RIST_PORT:-15100}"
RTMP_PORT="${RTMP_PORT:-19350}"
MTX_API_PORT="${MTX_API_PORT:-19997}"
TOKEN="e2e-token"
LOG_DIR="${LOG_DIR:-${BUILD_DIR}/e2e-logs}"
KEEP="${KEEP:-0}"
RUN_ID="$$"

# The sink has to be an address of this machine that is NOT loopback: the
# receiver's SSRF policy refuses 127.0.0.0/8 unconditionally. Default to the
# first global IPv4 on the box; traffic still never leaves the host.
if [[ -z "${SINK_HOST:-}" ]]; then
  SINK_HOST="$(ip -4 -o addr show scope global 2>/dev/null \
    | awk '{print $4}' | cut -d/ -f1 | head -1)"
fi
[[ -n "${SINK_HOST}" ]] || { echo "no non-loopback IPv4 found; set SINK_HOST" >&2; exit 2; }

RECEIVER_LOG="${LOG_DIR}/receiver.log"
SINK_LOG="${LOG_DIR}/mediamtx.log"
SAMPLE_LOG="${LOG_DIR}/samples.tsv"
PROOF_LOG="${LOG_DIR}/fixture-proof.txt"
mkdir -p "${LOG_DIR}" "${BUILD_DIR}/fixtures"
: > "${RECEIVER_LOG}"; : > "${SINK_LOG}"; : > "${SAMPLE_LOG}"; : > "${PROOF_LOG}"

receiver_pid=""
sink_pid=""
feed_pid=""

cleanup() {
  local rc=$?
  for pid in "${feed_pid}" "${receiver_pid}" "${sink_pid}"; do
    [[ -n "${pid}" ]] || continue
    kill -TERM "${pid}" 2>/dev/null || true
  done
  sleep 0.5
  for pid in "${feed_pid}" "${receiver_pid}" "${sink_pid}"; do
    [[ -n "${pid}" ]] || continue
    kill -KILL "${pid}" 2>/dev/null || true
  done
  if [[ "${KEEP}" == "1" ]]; then
    echo
    echo "KEEP=1: leaving pid(s) ${feed_pid:-} ${receiver_pid:-} ${sink_pid:-} running"
    return ${rc}
  fi
  # Confirm nothing this script started survives. A signalled process can take a
  # moment to leave (mediamtx closes its RTMP connections on the way out), so the
  # check polls instead of sampling once and reporting a process that is still
  # shutting down.
  local survivors="" pid waited
  for pid in "${feed_pid}" "${receiver_pid}" "${sink_pid}"; do
    [[ -n "${pid}" ]] || continue
    for waited in $(seq 1 20); do
      kill -0 "${pid}" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "${pid}" 2>/dev/null; then survivors+=" ${pid}"; fi
  done
  if [[ -n "${survivors}" ]]; then
    echo "WARNING: survived cleanup:${survivors}" >&2
  fi
  return ${rc}
}
trap cleanup EXIT INT TERM

die() { echo "$*" >&2; exit 2; }

# ---- preflight -------------------------------------------------------------
[[ -x "${BIN}" ]]  || die "receiver binary not built: ${BIN}  (cmake --build build)"
[[ -x "${FEED}" ]] || die "rist_feed not built: ${FEED}  (cmake --build build --target rist_feed)"
for bin in mediamtx gst-launch-1.0 ffprobe pv jq curl; do
  command -v "${bin}" >/dev/null 2>&1 || die "missing tool: ${bin}"
done
for port in "${CTRL_PORT}" "${RIST_PORT}" "${RTMP_PORT}" "${MTX_API_PORT}"; do
  if ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]${port}$"; then
    die "port ${port} already in use (another run?)"
  fi
done

echo "=== fixture ==="
if [[ ! -s "${FIXTURE}" ]]; then
  FIXTURE_FRAMES="${FIXTURE_FRAMES}" \
    "${REPO_ROOT}/test/tools/make-av1-ts-fixture.sh" "${FIXTURE}" \
    || die "fixture generation failed"
else
  echo "reusing ${FIXTURE}"
fi
# A reused fixture must be fed at its own frame count, not this script's
# default: the feed rate below is derived from frames/fps, and a fixture with
# half the assumed frames would be fed at half speed. An explicit
# FIXTURE_FRAMES still wins.
if [[ -z "${FIXTURE_FRAMES_GIVEN}" && -s "${FIXTURE}.meta" ]]; then
  meta_frames=$(sed -n 's/^frames=//p' "${FIXTURE}.meta")
  [[ -n "${meta_frames}" ]] && FIXTURE_FRAMES="${meta_frames}"
fi
fixture_bytes=$(wc -c < "${FIXTURE}")
echo "fixture: ${FIXTURE} (${fixture_bytes} bytes, ${FIXTURE_FRAMES} frames @ ${FIXTURE_FPS} fps)"

# ---- fixture proof ---------------------------------------------------------
{
  echo "--- ffprobe -show_streams ${FIXTURE} ---"
  echo "(ffprobe reports bin_data: FFmpeg's MPEG-TS demuxer has no mapping for"
  echo " AV1's private stream_type 0x06/AV1G, so it keeps the tag but not the"
  echo " codec. The gate below is tsdemux, which is the receiver's own demuxer.)"
  ffprobe -v error \
    -show_entries stream=index,codec_name,codec_type,profile,width,height,r_frame_rate \
    -of default=noprint_wrappers=1 "${FIXTURE}"
  echo
  echo "--- gst: filesrc ! tsparse ! tsdemux ! video/x-av1 (caps of the demuxed pad) ---"
  gst-launch-1.0 -v filesrc location="${FIXTURE}" ! tsparse ! tsdemux name=d \
    d. ! queue ! av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame \
    ! fakesink num-buffers=3 2>&1 \
    | grep -E "GstQueue:queue0.GstPad:(sink|src)|GstAV1Parse|ERROR|WARNING" | head -6
  echo
  echo "--- PMT as it exists in the file (stream_type + descriptors) ---"
  python3 - "${FIXTURE}" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
pmt_pid = None
for pos in range(0, len(data) - 188, 188):
    p = data[pos:pos + 188]
    if p[0] != 0x47:
        continue
    pid = ((p[1] & 0x1f) << 8) | p[2]
    pusi = bool(p[1] & 0x40)
    afc = (p[3] >> 4) & 3
    if not (afc & 1):
        continue
    off = 4 + (1 + p[4] if afc & 2 else 0)
    if not pusi:
        continue
    sec = p[off:]
    s = sec[1 + sec[0]:]
    slen = ((s[1] & 0xf) << 8) | s[2]
    # Section layout: table_id(1) section_length(2) then 5 bytes of
    # transport_stream_id/program_number + version + section numbers, then the
    # loop; the last 4 bytes are CRC-32.
    body = s[8:3 + slen - 4]
    if pid == 0 and s[0] == 0x00:
        i = 0
        while i + 4 <= len(body):
            prog = (body[i] << 8) | body[i + 1]
            if prog == 0:
                break
            pmt_pid = ((body[i + 2] & 0x1f) << 8) | body[i + 3]
            print(f"PAT: program={prog} PMT_PID={pmt_pid}")
            i += 4
        continue
    if pid == pmt_pid and s[0] == 0x02:
        pcr = ((body[0] & 0x1f) << 8) | body[1]
        info_len = ((body[2] & 0xf) << 8) | body[3]
        print(f"PMT: PCR_PID={pcr}")
        i = 4 + info_len
        while i + 5 <= len(body):
            st = body[i]
            epid = ((body[i + 1] & 0x1f) << 8) | body[i + 2]
            es_len = ((body[i + 3] & 0xf) << 8) | body[i + 4]
            desc = body[i + 5:i + 5 + es_len]
            print(f"  ES: stream_type=0x{st:02x} PID={epid} es_info_len={es_len}")
            j = 0
            while j + 2 <= len(desc):
                tag, ln = desc[j], desc[j + 1]
                payload = desc[j + 2:j + 2 + ln]
                if tag == 0x05:
                    print(f"    descriptor 0x05 registration: {payload!r}")
                elif tag == 0x80:
                    print(f"    descriptor 0x80 private (av1C): {payload.hex()}")
                j += 2 + ln
            i += 5 + es_len
        break
PY
} | tee "${PROOF_LOG}"

# Gate: the demuxed pad must carry video/x-av1. A TS that is really bin_data
# (the earlier invalid attempt) fails here and the run must not continue.
if ! grep -q "video/x-av1" "${PROOF_LOG}"; then
  echo "FIXTURE INVALID: tsdemux exposed no video/x-av1 pad" >&2
  exit 1
fi
if ! grep -q "stream_type=0x06" "${PROOF_LOG}" \
   || ! grep -q "registration: b'AV1G'" "${PROOF_LOG}"; then
  echo "FIXTURE INVALID: PMT is not the AV1 private mapping (0x06 + AV1G)" >&2
  exit 1
fi
echo "fixture: tsdemux exposes video/x-av1; PMT carries stream_type 0x06 + AV1G"

# ---- local RTMP sink -------------------------------------------------------
echo
echo "=== sink (MediaMTX) ==="
MTX_CONF="${LOG_DIR}/mediamtx-${RUN_ID}.yml"
cat > "${MTX_CONF}" <<EOF
logLevel: info
rtsp: no
hls: no
webrtc: no
srt: no
# MoQ binds a fixed global port (8892) and nothing here publishes MoQ; leaving it
# on makes a second harness run fail to start while another one is live.
moq: no
rtmp: yes
rtmpAddress: ${SINK_HOST}:${RTMP_PORT}
rtmpEncryption: "no"
api: yes
apiAddress: 127.0.0.1:${MTX_API_PORT}
paths:
  all_others:
EOF
( cd "${LOG_DIR}" && exec mediamtx "${MTX_CONF}" ) >> "${SINK_LOG}" 2>&1 &
sink_pid=$!
echo "mediamtx pid ${sink_pid} rtmp=${SINK_HOST}:${RTMP_PORT} api=127.0.0.1:${MTX_API_PORT}"
ready=0
for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.2
done
[[ "${ready}" -eq 1 ]] || { tail -20 "${SINK_LOG}" >&2; die "mediamtx API did not come up"; }

# ---- receiver --------------------------------------------------------------
echo
echo "=== receiver ==="
"${BIN}" --control-port "${CTRL_PORT}" --rist-port "${RIST_PORT}" \
  --token "${TOKEN}" --egress-allow-private >> "${RECEIVER_LOG}" 2>&1 &
receiver_pid=$!
echo "receiver pid ${receiver_pid} control=127.0.0.1:${CTRL_PORT} rist=${RIST_PORT}"
ready=0
for _ in $(seq 1 50); do
  if curl -sf -H "Authorization: Bearer ${TOKEN}" \
      "http://127.0.0.1:${CTRL_PORT}/status" >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.2
done
[[ "${ready}" -eq 1 ]] || { tail -20 "${RECEIVER_LOG}" >&2; die "receiver control API did not come up"; }

START_BODY=$(cat <<EOF
{
  "schema_version": 4,
  "session_id": "e2e-local-${RUN_ID}",
  "ingest": {"bandwidth": 8000, "rist_listen": "rist://@127.0.0.1:${RIST_PORT}"},
  "source": {"codec": "av1"},
  "outputs": [
    {"id": "local-rtmp", "type": "rtmp",
     "url": "rtmp://${SINK_HOST}:${RTMP_PORT}/live",
     "key_or_streamid": "e2e",
     "transcode": {"codec": "h264", "bitrate_kbps": 2000}}
  ]
}
EOF
)
echo "POST /start"
curl -s -X POST "http://127.0.0.1:${CTRL_PORT}/start" \
  -H "Authorization: Bearer ${TOKEN}" -H 'Content-Type: application/json' \
  -d "${START_BODY}" | tee "${LOG_DIR}/start-response.json"
echo

# ---- feed ------------------------------------------------------------------
feed_seconds=$(( FIXTURE_FRAMES / FIXTURE_FPS ))
rate=$(( fixture_bytes / feed_seconds ))
[[ "${rate}" -gt 0 ]] || rate=1
echo
echo "=== feeding ${fixture_bytes} bytes over RIST at ~${rate} B/s (${feed_seconds}s at 1x) ==="
( pv -q -L "${rate}" "${FIXTURE}" | "${FEED}" 127.0.0.1 "${RIST_PORT}" ) &
feed_pid=$!

sample() {  # $1 = elapsed seconds
  local stats sink path ready_str bytes_sent state
  stats=$(curl -s -H "Authorization: Bearer ${TOKEN}" \
            "http://127.0.0.1:${CTRL_PORT}/stats" 2>/dev/null)
  sink=$(curl -s "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" 2>/dev/null)
  path=$(jq -r '.items[0] | if . == null then "none" else .name end' <<<"${sink}" 2>/dev/null)
  ready_str=$(jq -r '.items[0].ready // false' <<<"${sink}" 2>/dev/null)
  bytes_sent=$(jq -r '.items[0].bytesReceived // 0' <<<"${sink}" 2>/dev/null)
  state=$(jq -r '.outputs[0].state // "?"' <<<"${stats}" 2>/dev/null)
  local rist_recv ts_in out_bytes dropped reconnects
  rist_recv=$(jq -r '.rist.received // 0' <<<"${stats}" 2>/dev/null)
  ts_in=$(jq -r '.ts_in.bytes_total // 0' <<<"${stats}" 2>/dev/null)
  out_bytes=$(jq -r '.outputs[0].bytes_sent // 0' <<<"${stats}" 2>/dev/null)
  dropped=$(jq -r '.outputs[0].transcode.frames_dropped // 0' <<<"${stats}" 2>/dev/null)
  reconnects=$(jq -r '.outputs[0].reconnects // 0' <<<"${stats}" 2>/dev/null)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$1" "${rist_recv}" "${ts_in}" "${state}" "${out_bytes}" "${dropped}" \
    "${reconnects}" "${path}:${ready_str}" "${bytes_sent}" \
    | tee -a "${SAMPLE_LOG}"
}

echo "t	rist_recv	ts_in	out_state	out_bytes_sent	frames_dropped	reconnects	sink_path:ready	sink_bytes_received"
pull_pid=""
PULL_FILE="${LOG_DIR}/pulled-from-sink.flv"
rm -f "${PULL_FILE}"
for t in $(seq 1 $(( feed_seconds + 4 ))); do
  sleep 1
  sample "${t}"
  # While the publisher is still live, read the stream back OUT of the RTMP
  # server: bytes alone do not prove a decodable stream.
  if [[ "${t}" -eq $(( feed_seconds - 3 )) && "${SKIP_PULL:-0}" != "1" ]]; then
    ( timeout 25 ffmpeg -v error -i "rtmp://${SINK_HOST}:${RTMP_PORT}/live/e2e" \
        -t 3 -c copy -y "${PULL_FILE}" >"${LOG_DIR}/pull.log" 2>&1 ) &
    pull_pid=$!
  fi
done
[[ -n "${pull_pid}" ]] && wait "${pull_pid}" 2>/dev/null

wait "${feed_pid}" 2>/dev/null
feed_pid=""

# ---- verdict ---------------------------------------------------------------
echo
echo "=== counter trace ==="
column -t -s $'\t' "${SAMPLE_LOG}"

first_sink_bytes=$(awk -F'\t' 'NR>1 && $9+0>0 {print $9; exit}' "${SAMPLE_LOG}")
last_sink_bytes=$(awk -F'\t' 'END{print $9+0}' "${SAMPLE_LOG}")
sink_growth=$(( last_sink_bytes - ${first_sink_bytes:-0} ))
max_ts_in=$(awk -F'\t' 'NR>1 && $3+0>m {m=$3+0} END{print m+0}' "${SAMPLE_LOG}")
max_rist=$(awk -F'\t' 'NR>1 && $2+0>m {m=$2+0} END{print m+0}' "${SAMPLE_LOG}")
out_running=$(awk -F'\t' 'NR>1 && $4=="running" {c++} END{print c+0}' "${SAMPLE_LOG}")
max_reconnects=$(awk -F'\t' 'NR>1 && $7+0>m {m=$7+0} END{print m+0}' "${SAMPLE_LOG}")

decoder_line=$(grep -a "\[decoder\] in=" "${RECEIVER_LOG}" | tail -1)
decoded=$(sed -n 's/.*decoded=\([0-9]*\).*/\1/p' <<<"${decoder_line}")
demux=$(sed -n 's/.*demux=\([0-9]*\).*/\1/p' <<<"${decoder_line}")
chunks=$(sed -n 's/.*in=\([0-9]*\) chunks.*/\1/p' <<<"${decoder_line}")
decoder_elem=$(sed -n 's/.*dec=\([a-z0-9_]*\).*/\1/p' <<<"${decoder_line}")
decoder_state=$(sed -n 's/.*state=\([A-Z]*\).*/\1/p' <<<"${decoder_line}")

echo
echo "=== decode stage (last [decoder] line) ==="
echo "${decoder_line:-<no decoder log line>}"
echo
echo "=== publish ==="
echo "sink path            $(tail -1 "${SAMPLE_LOG}" | cut -f8)"
echo "sink bytesReceived   ${first_sink_bytes:-0} -> ${last_sink_bytes} (growth ${sink_growth})"
echo "output state         running in ${out_running} sample(s), reconnects ${max_reconnects}"

pull_ok=0
if [[ -s "${PULL_FILE}" ]]; then
  pulled=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=codec_name,width,height,nb_read_frames \
    -of default=noprint_wrappers=1 "${PULL_FILE}" 2>/dev/null | tr '\n' ' ')
  echo "read back from sink  ${pulled:-<unprobeable>}($(wc -c < "${PULL_FILE}") B pulled)"
  # Any probeable H.264 video with frames read back proves the publish carried
  # a real elementary stream (the size is printed, not asserted: the fixture
  # resolution is a harness input).
  [[ "${pulled}" == *codec_name=h264* && "${pulled}" == *nb_read_frames=* ]] && pull_ok=1
else
  echo "read back from sink  nothing pulled (see ${LOG_DIR}/pull.log)"
fi

verdict=0
if [[ "${max_rist}" -eq 0 ]]; then
  echo "VERDICT: STALLED at RIST ingest (no packets received)"
  verdict=1
elif [[ "${max_ts_in}" -eq 0 ]]; then
  echo "VERDICT: STALLED at the TS ring (RIST packets received, 0 bytes reached the ring)"
  verdict=1
elif [[ "${chunks:-0}" -eq 0 ]]; then
  echo "VERDICT: STALLED at the decode appsrc (${max_ts_in} ring bytes, 0 chunks fed)"
  verdict=1
elif [[ "${demux:-0}" -eq 0 ]]; then
  echo "VERDICT: STALLED at tsdemux (${chunks} chunks fed, no demuxed buffers)"
  verdict=1
elif [[ "${decoded:-0}" -eq 0 ]]; then
  echo "VERDICT: STALLED at ${decoder_elem:-the decoder} (${demux} demuxed buffers, 0 decoded)"
  verdict=1
elif [[ "${max_reconnects}" -ne 0 ]]; then
  echo "VERDICT: RECONNECTED — the sink received ${sink_growth} B, but the output rebuilt"
  echo "         its attempt ${max_reconnects} time(s); the run never held one connection."
  verdict=1
elif [[ "${sink_growth}" -lt 32768 ]]; then
  echo "VERDICT: decode OK (${decoded} frames, ${decoder_elem}), but STALLED before RTMP:"
  echo "         the sink received ${sink_growth} B (handshake only); output state was"
  echo "         '$(tail -1 "${SAMPLE_LOG}" | cut -f4)' with bytes_sent"
  echo "         $(tail -1 "${SAMPLE_LOG}" | cut -f5) — that counter is NV12 frames"
  echo "         pushed into the video appsrc, not bytes on the wire."
  verdict=1
elif [[ "${pull_ok}" -ne 1 ]]; then
  echo "VERDICT: PARTIAL — sink received ${sink_growth} B but the pulled stream was not"
  echo "         a probeable H.264 1280x720 stream; treat the publish as unproven."
  verdict=1
else
  echo "VERDICT: PUBLISHES — ${decoded} frames decoded by ${decoder_elem}, sink received ${sink_growth} B,"
  echo "         and a decodable H.264 stream reads back out of the RTMP server"
fi

echo
echo "=== receiver log: decode/output transitions ==="
grep -aE "\[decoder\]|\[output|\[rist\]|bus error|stalled|running" "${RECEIVER_LOG}" \
  | grep -avE "^[0-9.]+.*\[INFO\]" | tail -40
echo
echo "logs: ${RECEIVER_LOG} ${SINK_LOG} ${PROOF_LOG} ${SAMPLE_LOG}"
exit ${verdict}
