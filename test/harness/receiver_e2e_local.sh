#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# receiver_e2e_local.sh — end-to-end exercise of the WHOLE receiver binary with
# no rig and no network dependency:
#
#   AV1-in-MPEG-TS fixture (with an AAC-LC track) -> rist_feed (real
#   RIST/ADVANCED ingest) -> open-broadcast-receiver -> POST /start ->
#   decode (vaav1dec) -> transcode (vah264enc) -> flvmux -> rtmp2sink ->
#   local MediaMTX
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
#                         video/x-av1 pad, and an audio/mpeg pad that aacparse
#                         accepts when the fixture carries audio (and the PMT
#                         bytes: stream_type 0x06 with the AV1G registration
#                         descriptor and av1C).
#   * decode progress   — the decoder's own staged counters in its log lines
#                         (in chunks / demux / decoded / sink / state).
#   * publish           — the RTMP SERVER's byte counter for the path
#                         (MediaMTX /v3/paths/list bytesReceived), which cannot
#                         be inflated by the receiver. The output's own
#                         `bytes_sent` and `bytes_published_total` are the same
#                         sink-pad measurement, while `bytes_pushed` is the NV12
#                         volume fed to the video appsrc, which grows even when
#                         nothing reaches RTMP; only the server's counter proves
#                         the bytes arrived.
#   * stream readback   — the stream is pulled back OUT of the RTMP server and
#                         probed with `ffprobe -count_packets`: real packet
#                         counts for h264 video, plus for aac audio when the
#                         fixture carries it. A stream that is declared but
#                         carries 0 packets fails the run.
#   * audio at the muxer — when the fixture carries AAC, the run fails unless the
#                         receiver log shows the muxer's audio pad receiving
#                         AAC-LC 48 kHz stereo raw with codec_data 1190, and
#                         unless the 4 s no-buffer probe did NOT retire that pad.
#                         A source with no AAC must instead never hand the muxer
#                         an audio pad at all.
#
# Usage:
#   test/harness/receiver_e2e_local.sh
#
# Env overrides: BIN FEED FIXTURE FIXTURE_FRAMES FIXTURE_AUDIO CTRL_PORT
#                RIST_PORT RTMP_PORT MTX_API_PORT SINK_HOST LOG_DIR
#                KEEP=1 (leave this run's processes up and print their pids;
#                        without it every started process is stopped on exit)
#
# Exit status: 0 = decode + publish both proven, 1 = a stage stopped (the log
# says which), 2 = the harness could not run (missing binary/element/port).

set -uo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

# A process started through sudo belongs to root, and this user cannot signal it
# afterwards -- which is how a previous run left mediamtx holding its port.
# Re-exec as the invoking user so every pid the trap records is signalable.
if [[ "${EUID}" -eq 0 && -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
  invoking_home=$(getent passwd "${SUDO_USER}" | cut -d: -f6)
  exec setpriv --reuid="$(id -u "${SUDO_USER}")" \
    --regid="$(id -g "${SUDO_USER}")" --init-groups \
    env HOME="${invoking_home}" "${SCRIPT_PATH}" "$@"
fi

REPO_ROOT="$(cd "$(dirname "${SCRIPT_PATH}")/../.." && pwd)"
BIN="${BIN:-${REPO_ROOT}/build/open-broadcast-receiver}"
FEED="${FEED:-${REPO_ROOT}/build/test/rist_feed}"
BUILD_DIR="${REPO_ROOT}/build"
# The name states the tracks the fixture carries: the audio checks below are
# assertions about this file, so a reused video-only file under an audio name
# would quietly make them vacuous.
FIXTURE="${FIXTURE:-${BUILD_DIR}/fixtures/e2e-av1-aac.ts}"
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

started_pids=()
started_pgids=()
started_names=()

# Starts NAME CMD... in its own process group and records the group, so the
# trap can stop a pipeline's children too (`pv | rist_feed` is one job with two
# processes). setsid does not fork here: the background child is not a process
# group leader, so $! is both the pid and the pgid.
start_bg() {
  local name=$1 pid pgid
  shift
  setsid "$@" &
  pid=$!
  pgid=$(ps -o pgid= -p "${pid}" 2>/dev/null | tr -d ' ')
  started_pids+=("${pid}")
  started_pgids+=("${pgid:-${pid}}")
  started_names+=("${name}")
  LAST_STARTED_PID=${pid}
}

stop_started() {
  local i pid pgid own_pgid waited survivors=""
  own_pgid=$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')
  for i in "${!started_pids[@]}"; do
    pid=${started_pids[$i]}
    pgid=${started_pgids[$i]}
    kill -0 "${pid}" 2>/dev/null || continue
    if [[ -n "${pgid}" && "${pgid}" != "${own_pgid}" ]]; then
      kill -TERM -- "-${pgid}" 2>/dev/null || true
    fi
    kill -TERM "${pid}" 2>/dev/null || true
  done
  # mediamtx closes its RTMP connections on the way out, so poll rather than
  # sample once and report a process that is still shutting down.
  for waited in $(seq 1 30); do
    survivors=""
    for pid in "${started_pids[@]}"; do
      kill -0 "${pid}" 2>/dev/null && survivors+=" ${pid}"
    done
    [[ -z "${survivors}" ]] && return 0
    sleep 0.1
  done
  for i in "${!started_pids[@]}"; do
    pid=${started_pids[$i]}
    pgid=${started_pgids[$i]}
    kill -0 "${pid}" 2>/dev/null || continue
    if [[ -n "${pgid}" && "${pgid}" != "${own_pgid}" ]]; then
      kill -KILL -- "-${pgid}" 2>/dev/null || true
    fi
    kill -KILL "${pid}" 2>/dev/null || true
  done
  sleep 0.2
  survivors=""
  for pid in "${started_pids[@]}"; do
    kill -0 "${pid}" 2>/dev/null && survivors+=" ${pid}"
  done
  if [[ -n "${survivors}" ]]; then
    echo "WARNING: survived cleanup:${survivors}" >&2
  fi
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [[ "${KEEP}" == "1" ]]; then
    echo
    local i
    for i in "${!started_pids[@]}"; do
      kill -0 "${started_pids[$i]}" 2>/dev/null || continue
      echo "KEEP=1: leaving ${started_names[$i]} pid ${started_pids[$i]} (pgid ${started_pgids[$i]}) running"
    done
  else
    stop_started
  fi
  exit "${rc}"
}

# The EXIT trap does the stopping for every path, including `die` partway
# through; the signal traps turn INT/TERM into an exit so it runs once.
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

die() { echo "$*" >&2; exit 2; }

# The pid listening on a TCP port, when this user can see it.
port_pid() {
  ss -ltnpH "sport = :$1" 2>/dev/null \
    | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -1
}

port_busy() {
  [[ -n "$(ss -ltnH "sport = :$1" 2>/dev/null)" ]]
}

# ---- preflight -------------------------------------------------------------
[[ -x "${BIN}" ]]  || die "receiver binary not built: ${BIN}  (cmake --build build)"
[[ -x "${FEED}" ]] || die "rist_feed not built: ${FEED}  (cmake --build build --target rist_feed)"
for bin in mediamtx gst-launch-1.0 ffprobe pv jq curl; do
  command -v "${bin}" >/dev/null 2>&1 || die "missing tool: ${bin}"
done
for port in "${CTRL_PORT}" "${RIST_PORT}" "${RTMP_PORT}" "${MTX_API_PORT}"; do
  port_busy "${port}" || continue
  holder=$(port_pid "${port}")
  if [[ -n "${holder}" ]]; then
    die "port ${port} is held by pid ${holder} ($(ps -o comm= -p "${holder}" 2>/dev/null || echo unknown)), which this run did not start; stop it or override the port"
  fi
  die "port ${port} is held by a process this user cannot inspect (another user or a container); stop it or override the port"
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
  if ffprobe -v error -select_streams a -show_entries stream=codec_name \
      -of csv=p=0 "${FIXTURE}" 2>/dev/null | grep -q aac; then
    echo
    echo "--- gst: audio pad of the same TS (aacparse accepts it) ---"
    gst-launch-1.0 -v filesrc location="${FIXTURE}" ! tsparse ! tsdemux name=d \
      d. ! queue ! aacparse ! audio/mpeg,mpegversion=4,stream-format=adts,framed=true \
      ! fakesink num-buffers=3 2>&1 \
      | grep -E "GstAacParse|GstQueue:queue0.GstPad:(sink|src)|ERROR|WARNING" | head -4
  fi
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

# The audio checks later in this script are assertions about THIS file, so the
# file's tracks are checked here against what the run expects. A video-only
# fixture reused under the default name would otherwise make them vacuous.
if [[ "${FIXTURE_AUDIO:-1}" != "0" ]]; then
  if ! grep -q "GstAacParse:aacparse0.GstPad:src: caps = audio/mpeg" "${PROOF_LOG}"; then
    echo "FIXTURE INVALID: audio is expected (FIXTURE_AUDIO!=0) but ${FIXTURE}" >&2
    echo "                 exposes no AAC pad; regenerate it or set FIXTURE_AUDIO=0" >&2
    exit 1
  fi
  echo "fixture: AAC track present — $(grep -m1 'GstAacParse:aacparse0.GstPad:src' "${PROOF_LOG}" | sed 's/.*caps = //')"
fi

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
start_bg mediamtx bash -c 'cd "$1" && exec mediamtx "$2"' _ \
  "${LOG_DIR}" "${MTX_CONF}" >> "${SINK_LOG}" 2>&1
sink_pid=${LAST_STARTED_PID}
echo "mediamtx pid ${sink_pid} rtmp=${SINK_HOST}:${RTMP_PORT} api=127.0.0.1:${MTX_API_PORT}"
ready=0
for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.2
done
[[ "${ready}" -eq 1 ]] || { tail -20 "${SINK_LOG}" >&2; die "mediamtx API did not come up"; }
holder=$(port_pid "${MTX_API_PORT}")
[[ "${holder}" == "${sink_pid}" ]] \
  || die "mediamtx API port ${MTX_API_PORT} is held by pid ${holder:-unknown}, not this run's ${sink_pid}"

# ---- receiver --------------------------------------------------------------
echo
echo "=== receiver ==="
start_bg receiver "${BIN}" --control-port "${CTRL_PORT}" --rist-port "${RIST_PORT}" \
  --token "${TOKEN}" --egress-allow-private >> "${RECEIVER_LOG}" 2>&1
receiver_pid=${LAST_STARTED_PID}
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
start_bg feed env FIXTURE="${FIXTURE}" FEED="${FEED}" RATE="${rate}" RIST_PORT="${RIST_PORT}" \
  bash -c 'pv -q -L "${RATE}" "${FIXTURE}" | "${FEED}" 127.0.0.1 "${RIST_PORT}"'
feed_pid=${LAST_STARTED_PID}

sample() {  # $1 = elapsed seconds
  local stats sink path ready_str sink_bytes state
  stats=$(curl -s -H "Authorization: Bearer ${TOKEN}" \
            "http://127.0.0.1:${CTRL_PORT}/stats" 2>/dev/null)
  sink=$(curl -s "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" 2>/dev/null)
  path=$(jq -r '.items[0] | if . == null then "none" else .name end' <<<"${sink}" 2>/dev/null)
  ready_str=$(jq -r '.items[0].ready // false' <<<"${sink}" 2>/dev/null)
  sink_bytes=$(jq -r '.items[0].bytesReceived // 0' <<<"${sink}" 2>/dev/null)
  state=$(jq -r '.outputs[0].state // "?"' <<<"${stats}" 2>/dev/null)
  local rist_recv ts_in out_bytes dropped reconnects
  rist_recv=$(jq -r '.rist.received // 0' <<<"${stats}" 2>/dev/null)
  ts_in=$(jq -r '.ts_in.bytes_total // 0' <<<"${stats}" 2>/dev/null)
  out_bytes=$(jq -r '.outputs[0].bytes_published_total // 0' <<<"${stats}" 2>/dev/null)
  dropped=$(jq -r '.outputs[0].transcode.frames_dropped // 0' <<<"${stats}" 2>/dev/null)
  reconnects=$(jq -r '.outputs[0].reconnects // 0' <<<"${stats}" 2>/dev/null)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$1" "${rist_recv}" "${ts_in}" "${state}" "${out_bytes}" "${dropped}" \
    "${reconnects}" "${path}:${ready_str}" "${sink_bytes}" \
    | tee -a "${SAMPLE_LOG}"
}

echo "t	rist_recv	ts_in	out_state	out_bytes_published	frames_dropped	reconnects	sink_path:ready	sink_bytes_received"
pull_pid=""
PULL_FILE="${LOG_DIR}/pulled-from-sink.flv"
rm -f "${PULL_FILE}"
for t in $(seq 1 $(( feed_seconds + 4 ))); do
  sleep 1
  sample "${t}"
  # While the publisher is still live, read the stream back OUT of the RTMP
  # server: bytes alone do not prove a decodable stream.
  if [[ "${t}" -eq $(( feed_seconds - 3 )) && "${SKIP_PULL:-0}" != "1" ]]; then
    start_bg pull timeout 25 ffmpeg -v error -i "rtmp://${SINK_HOST}:${RTMP_PORT}/live/e2e" \
      -t 3 -c copy -y "${PULL_FILE}" >>"${LOG_DIR}/pull.log" 2>&1
    pull_pid=${LAST_STARTED_PID}
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
# Whether the fixture carries audio decides whether the readback has to prove an
# AAC stream: a video-only ingest publishing without audio is correct, and the
# point of the check is that audio is not silently lost when the source has it.
audio_expected=0
if ffprobe -v error -select_streams a -show_entries stream=codec_name \
    -of csv=p=0 "${FIXTURE}" 2>/dev/null | grep -q aac; then
  audio_expected=1
fi
video_packets=0
audio_packets=0
if [[ -s "${PULL_FILE}" ]]; then
  streams=$(ffprobe -v error -count_packets \
    -show_entries stream=index,codec_type,codec_name,width,height,nb_read_packets \
    -of json "${PULL_FILE}" 2>/dev/null)
  echo "read back from sink  ($(wc -c < "${PULL_FILE}") B pulled, ffprobe -count_packets)"
  jq -r '.streams[] | "  stream \(.index): \(.codec_type)/\(.codec_name)" +
           (if .width then " \(.width)x\(.height)" else "" end) +
           " packets=\(.nb_read_packets // "?")"' <<<"${streams}" 2>/dev/null
  video_packets=$(jq '[.streams[] | select(.codec_type=="video" and .codec_name=="h264")
                       | (.nb_read_packets|tonumber)] | add // 0' <<<"${streams}" 2>/dev/null)
  audio_packets=$(jq '[.streams[] | select(.codec_type=="audio" and .codec_name=="aac")
                       | (.nb_read_packets|tonumber)] | add // 0' <<<"${streams}" 2>/dev/null)
  video_packets=${video_packets:-0}
  audio_packets=${audio_packets:-0}
  # Packets, not streams: a declared-but-empty stream reads back with
  # nb_read_packets=0 and proves nothing left the receiver.
  if [[ "${video_packets}" -gt 0 ]] \
     && { [[ "${audio_expected}" -eq 0 ]] || [[ "${audio_packets}" -gt 0 ]]; }; then
    pull_ok=1
  fi
else
  echo "read back from sink  nothing pulled (see ${LOG_DIR}/pull.log)"
fi

verdict=0
# What the muxer was actually handed. The readback proves an AAC stream exists;
# this proves the branch produced the declared caps rather than something the
# destination cannot carry, and that the audio pad was not retired by the 4 s
# no-buffer probe (which is the path a missing or stuck audio branch takes).
audio_mux_ok=1
if [[ "${audio_expected}" -eq 1 ]]; then
  mux_audio_line=$(grep -am1 "muxer audio in:" "${RECEIVER_LOG}")
  echo
  echo "=== muxer audio input (from the receiver log) ==="
  echo "${mux_audio_line:-  (no 'muxer audio in' line: the audio branch never delivered a buffer)}"
  if [[ -z "${mux_audio_line}" ]] \
     || ! grep -q "stream-format=(string)raw" <<<"${mux_audio_line}" \
     || ! grep -q "rate=(int)48000" <<<"${mux_audio_line}" \
     || ! grep -q "channels=(int)2" <<<"${mux_audio_line}" \
     || ! grep -q "codec_data=(buffer)1190" <<<"${mux_audio_line}"; then
    audio_mux_ok=0
  fi
  if grep -aq "audio pad received no buffers" "${RECEIVER_LOG}"; then
    echo "  the 4 s probe retired the muxer's audio pad: no AAC reached it"
    audio_mux_ok=0
  fi
else
  # No audio in the source: the only correct shape is no muxer audio pad at all.
  if grep -aq "source TS carries audio" "${RECEIVER_LOG}"; then
    echo "a video-only source still handed the muxer an audio pad" >&2
    audio_mux_ok=0
  fi
fi
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
  echo "         '$(tail -1 "${SAMPLE_LOG}" | cut -f4)' with bytes_published_total"
  echo "         $(tail -1 "${SAMPLE_LOG}" | cut -f5) — the output handed the sink pad"
  echo "         that much and the destination still took only the handshake.
"
  verdict=1
elif [[ "${audio_mux_ok}" -ne 1 ]]; then
  echo "VERDICT: PARTIAL — the stream published, but the audio branch did not hand"
  echo "         the muxer AAC-LC 48 kHz stereo raw with codec_data 1190 (see the"
  echo "         muxer audio input above); treat the audio path as unproven."
  verdict=1
elif [[ "${pull_ok}" -ne 1 ]]; then
  echo "VERDICT: PARTIAL — sink received ${sink_growth} B but the pulled stream did not"
  echo "         read back as h264 video (${video_packets} packets) plus the fixture's"
  echo "         AAC audio (${audio_packets} packets, expected when the fixture carries"
  echo "         audio); treat the publish as unproven."
  verdict=1
else
  echo "VERDICT: PUBLISHES — ${decoded} frames decoded by ${decoder_elem}, sink received ${sink_growth} B,"
  if [[ "${audio_expected}" -eq 1 ]]; then
    echo "         and BOTH streams read back out of the RTMP server:"
    echo "         h264 video ${video_packets} packets, aac audio ${audio_packets} packets"
  else
    echo "         and an h264 stream (${video_packets} packets) reads back out of the RTMP server"
  fi
fi

echo
echo "=== receiver log: decode/output transitions ==="
grep -aE "\[decoder\]|\[output|\[rist\]|bus error|stalled|running" "${RECEIVER_LOG}" \
  | grep -avE "^[0-9.]+.*\[INFO\]" | tail -40
echo
echo "logs: ${RECEIVER_LOG} ${SINK_LOG} ${PROOF_LOG} ${SAMPLE_LOG}"
exit ${verdict}
