#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# receiver_e2e_guard.sh — CTest guard for the receiver's real end-to-end path:
#
#   AV1-in-MPEG-TS fixture -> rist_feed (real RIST/ADVANCED ingest) ->
#   open-broadcast-receiver -> POST /start -> vaav1dec ->
#   vah264enc -> flvmux -> rtmp2sink -> local MediaMTX
#
# It is registered as `receiver_e2e_guard` and is the regression gate for the
# four things that must never silently break:
#
#   1. the decoder reaches PLAYING,
#   2. it decoded more than zero frames,
#   3. the decoding element is vaav1dec (the VA-API decoder), not a software
#      fallback,
#   4. the output reports `running` and the RTMP server's own byte counter for
#      the path grows.
#
# Everything is local: the RIST sender, the receiver and the RTMP sink run on
# this machine. The only non-loopback hop is receiver -> sink, because the
# receiver's egress policy refuses 127.0.0.0/8 even with --egress-allow-private;
# that address is still one of this box's own interfaces, so no traffic leaves
# the host and no network access is required.
#
# Every port is picked from the set of ports currently free, so two runs (or a
# run next to the hand harness) cannot collide. Every process is started in its
# own process group and stopped on exit, including on SIGTERM from CTest's
# timeout.
#
# Usage:
#   receiver_e2e_guard.sh <receiver-binary> <rist_feed-binary> <fixture.ts>
#
# Env overrides: LOG_DIR SINK_HOST FIXTURE_FRAMES
#
# Exit status: 0 = all four assertions held, 1 = a stage regressed (the log
# names which), 77 = SKIP (no usable VA-API render node, missing tool, no
# non-loopback address to publish to).

set -uo pipefail

BIN="${1:?usage: receiver_e2e_guard.sh <receiver> <rist_feed> <fixture.ts>}"
FEED="${2:?usage: receiver_e2e_guard.sh <receiver> <rist_feed> <fixture.ts>}"
FIXTURE="${3:?usage: receiver_e2e_guard.sh <receiver> <rist_feed> <fixture.ts>}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOKEN="e2e-guard-token"
AUTH_HEADER="Authorization: Bearer ${TOKEN}"
FIXTURE_FPS=30
# 3 s at 30 fps: long enough for two decoder stats ticks (2 s period) and for
# RTMP to flush, short enough that the whole guard is a few seconds of media.
FIXTURE_FRAMES="${FIXTURE_FRAMES:-90}"
FIXTURE_WIDTH=640
FIXTURE_HEIGHT=360
FIXTURE_BITRATE=800
LOG_DIR="${LOG_DIR:-$(dirname "${FIXTURE}")/e2e-guard-logs}"

skip() {
  printf 'SKIP: %s\n' "$*"
  exit 77
}

for tool in mediamtx gst-launch-1.0 gst-inspect-1.0 ffprobe pv curl jq ss; do
  command -v "${tool}" >/dev/null 2>&1 || skip "missing tool: ${tool}"
done
[ -x "${BIN}" ] || { echo "receiver binary not built: ${BIN}" >&2; exit 1; }
[ -x "${FEED}" ] || { echo "rist_feed not built: ${FEED}" >&2; exit 1; }

# vaav1dec needs a VA-API render node. A box without one cannot run this test at
# all, so it skips rather than failing on an environment limitation.
render_node=""
for node in /dev/dri/renderD*; do
  [ -r "${node}" ] && [ -w "${node}" ] && { render_node="${node}"; break; }
done
[ -n "${render_node}" ] \
  || skip "no usable /dev/dri render node (vaav1dec needs VA-API, and there is no software AV1 decoder in the receiver's element list)"

for element in vaav1dec vah264enc av1parse mpegtsmux tsdemux flvmux rtmp2sink; do
  gst-inspect-1.0 "${element}" >/dev/null 2>&1 \
    || skip "GStreamer element ${element} is not installed"
done

# The sink has to be an address of this machine that is NOT loopback: the
# receiver's SSRF policy refuses 127.0.0.0/8 unconditionally.
if [ -z "${SINK_HOST:-}" ]; then
  SINK_HOST="$(ip -4 -o addr show scope global 2>/dev/null \
    | awk '{print $4}' | cut -d/ -f1 | head -1)"
fi
[ -n "${SINK_HOST:-}" ] \
  || skip "no non-loopback IPv4 address to publish to (the receiver refuses loopback destinations by policy)"

mkdir -p "${LOG_DIR}" "$(dirname "${FIXTURE}")"
LOG_DIR="$(cd "${LOG_DIR}" && pwd)"
FIXTURE="$(cd "$(dirname "${FIXTURE}")" && pwd)/$(basename "${FIXTURE}")"
RECEIVER_LOG="${LOG_DIR}/receiver.log"
SINK_LOG="${LOG_DIR}/mediamtx.log"
MTX_CONF="${LOG_DIR}/mediamtx.yml"
START_BODY_FILE="${LOG_DIR}/start.json"
: > "${RECEIVER_LOG}"
: > "${SINK_LOG}"

started_pids=()
started_pgids=()
started_names=()

cleanup() {
  local rc=$? i pid pgid own_pgid waited survivors
  trap - EXIT INT TERM
  own_pgid=$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')
  for i in "${!started_pids[@]}"; do
    pid=${started_pids[$i]}
    pgid=${started_pgids[$i]}
    kill -0 "${pid}" 2>/dev/null || continue
    if [ -n "${pgid}" ] && [ "${pgid}" != "${own_pgid}" ]; then
      kill -TERM -- "-${pgid}" 2>/dev/null || true
    fi
    kill -TERM "${pid}" 2>/dev/null || true
  done
  for waited in $(seq 1 40); do
    survivors=""
    for pid in "${started_pids[@]}"; do
      kill -0 "${pid}" 2>/dev/null && survivors+=" ${pid}"
    done
    [ -z "${survivors}" ] && break
    sleep 0.1
  done
  if [ -n "${survivors}" ]; then
    for i in "${!started_pids[@]}"; do
      pid=${started_pids[$i]}
      pgid=${started_pgids[$i]}
      kill -0 "${pid}" 2>/dev/null || continue
      if [ -n "${pgid}" ] && [ "${pgid}" != "${own_pgid}" ]; then
        kill -KILL -- "-${pgid}" 2>/dev/null || true
      fi
      kill -KILL "${pid}" 2>/dev/null || true
    done
    sleep 0.2
    survivors=""
    for pid in "${started_pids[@]}"; do
      kill -0 "${pid}" 2>/dev/null && survivors+=" ${pid}"
    done
    [ -z "${survivors}" ] || echo "WARNING: survived cleanup:${survivors}" >&2
  fi
  exit "${rc}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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

port_free() {  # $1 = port; free for both TCP listen and UDP bind
  ! ss -ltnH "sport = :$1" 2>/dev/null | grep -q . \
    && ! ss -lunH "sport = :$1" 2>/dev/null | grep -q .
}

pick_port() {  # $1 = starting candidate
  local p=$1 tries=0
  while [ "${tries}" -lt 400 ]; do
    if port_free "${p}"; then
      echo "${p}"
      return 0
    fi
    p=$(( p % 20000 + 20001 ))
    tries=$(( tries + 1 ))
  done
  return 1
}

base=$(( 21000 + ($$ * 13) % 8000 ))
CTRL_PORT=$(pick_port "${base}")            || skip "no free TCP port for the receiver control API"
RIST_PORT=$(pick_port $(( base + 1 )))      || skip "no free UDP port for RIST ingest"
RTMP_PORT=$(pick_port $(( base + 2 )))      || skip "no free TCP port for the RTMP sink"
MTX_API_PORT=$(pick_port $(( base + 3 )))   || skip "no free TCP port for the MediaMTX API"

# ---- fixture ---------------------------------------------------------------
# Generated once per build directory and reused; only the AV1 elementary
# stream is carried (no audio) so this guard measures decode and publish and
# nothing else. When no AV1 encoder is installed the checked-in fixture in
# test/fixtures/ is used instead (see test/fixtures/README.md).
if [ ! -s "${FIXTURE}" ]; then
  if gst-inspect-1.0 vaav1enc >/dev/null 2>&1 || gst-inspect-1.0 av1enc >/dev/null 2>&1; then
    echo "generating fixture ${FIXTURE}"
    FIXTURE_WIDTH="${FIXTURE_WIDTH}" FIXTURE_HEIGHT="${FIXTURE_HEIGHT}" \
    FIXTURE_FRAMES="${FIXTURE_FRAMES}" FIXTURE_BITRATE="${FIXTURE_BITRATE}" \
    FIXTURE_AUDIO=0 \
      "${REPO_ROOT}/test/tools/make-av1-ts-fixture.sh" "${FIXTURE}" \
      || { echo "fixture generation failed" >&2; exit 1; }
  else
    shipped="${REPO_ROOT}/test/fixtures/av1-640x360.ts"
    [ -s "${shipped}" ] \
      || skip "no AV1 encoder to generate a fixture and ${shipped} is absent"
    echo "no AV1 encoder installed; using checked-in ${shipped}"
    cp "${shipped}" "${FIXTURE}"
    [ -s "${shipped}.meta" ] && cp "${shipped}.meta" "${FIXTURE}.meta"
  fi
fi
[ -s "${FIXTURE}" ] || { echo "fixture ${FIXTURE} is empty" >&2; exit 1; }

reused_frames=$(sed -n 's/^frames=//p' "${FIXTURE}.meta" 2>/dev/null || true)
[ -n "${reused_frames}" ] && FIXTURE_FRAMES="${reused_frames}"
fixture_bytes=$(wc -c < "${FIXTURE}")
feed_seconds=$(( FIXTURE_FRAMES / FIXTURE_FPS ))
[ "${feed_seconds}" -gt 0 ] || feed_seconds=1
rate=$(( fixture_bytes / feed_seconds ))
[ "${rate}" -gt 0 ] || rate=1
echo "fixture: ${FIXTURE} (${fixture_bytes} bytes, ${FIXTURE_FRAMES} frames @ ${FIXTURE_FPS} fps)"

# ---- local RTMP sink -------------------------------------------------------
cat > "${MTX_CONF}" <<EOF
logLevel: info
rtsp: no
hls: no
webrtc: no
srt: no
moq: no
rtmp: yes
rtmpAddress: ${SINK_HOST}:${RTMP_PORT}
rtmpEncryption: "no"
api: yes
apiAddress: 127.0.0.1:${MTX_API_PORT}
paths:
  all_others:
EOF
start_bg mediamtx bash -c 'cd "$1" && exec mediamtx "$2"' _ "${LOG_DIR}" "${MTX_CONF}" \
  >> "${SINK_LOG}" 2>&1
sink_pid=${LAST_STARTED_PID}
echo "mediamtx pid ${sink_pid} rtmp=${SINK_HOST}:${RTMP_PORT} api=127.0.0.1:${MTX_API_PORT}"

ready=0
for _ in $(seq 1 60); do
  if curl -sf "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.1
done
[ "${ready}" -eq 1 ] || { tail -20 "${SINK_LOG}" >&2; echo "mediamtx API did not come up" >&2; exit 1; }

# ---- receiver --------------------------------------------------------------
start_bg receiver "${BIN}" --control-port "${CTRL_PORT}" --rist-port "${RIST_PORT}" \
  --token "${TOKEN}" --egress-allow-private >> "${RECEIVER_LOG}" 2>&1
receiver_pid=${LAST_STARTED_PID}
echo "receiver pid ${receiver_pid} control=127.0.0.1:${CTRL_PORT} rist=${RIST_PORT}"

ready=0
for _ in $(seq 1 60); do
  if curl -sf -H "${AUTH_HEADER}" \
      "http://127.0.0.1:${CTRL_PORT}/status" >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.1
done
[ "${ready}" -eq 1 ] || { tail -20 "${RECEIVER_LOG}" >&2; echo "receiver control API did not come up" >&2; exit 1; }

cat > "${START_BODY_FILE}" <<EOF
{
  "schema_version": 4,
  "session_id": "e2e-guard-$$",
  "ingest": {"bandwidth": 8000, "rist_listen": "rist://@127.0.0.1:${RIST_PORT}"},
  "source": {"codec": "av1"},
  "outputs": [
    {"id": "local-rtmp", "type": "rtmp",
     "url": "rtmp://${SINK_HOST}:${RTMP_PORT}/live",
     "key_or_streamid": "guard",
     "transcode": {"codec": "h264", "bitrate_kbps": 1000}}
  ]
}
EOF
curl -sf -X POST "http://127.0.0.1:${CTRL_PORT}/start" \
  -H "${AUTH_HEADER}" -H 'Content-Type: application/json' \
  -d @"${START_BODY_FILE}" >/dev/null \
  || { tail -20 "${RECEIVER_LOG}" >&2; echo "POST /start was refused" >&2; exit 1; }

# ---- feed ------------------------------------------------------------------
echo "feeding ${fixture_bytes} bytes over RIST at ~${rate} B/s (${feed_seconds}s)"
start_bg feed env FIXTURE="${FIXTURE}" FEED="${FEED}" RATE="${rate}" RIST_PORT="${RIST_PORT}" \
  bash -c 'pv -q -L "${RATE}" "${FIXTURE}" | "${FEED}" 127.0.0.1 "${RIST_PORT}"' \
  >> "${LOG_DIR}/feed.log" 2>&1
feed_pid=${LAST_STARTED_PID}

# ---- verdict ---------------------------------------------------------------
decoder_line=""
decoded=0
in_kib=0
fixture_kib=$(( fixture_bytes / 1024 ))
decoder_elem=""
decoder_state=""
decoder_fallback=1
out_running=0
sink_bytes=0
out_bytes_sent=0
out_bytes_pushed=0
feed_done=0
deadline=$(( SECONDS + 40 ))

while [ "${SECONDS}" -lt "${deadline}" ]; do
  decoder_line=$(grep -a '\[decoder\] in=' "${RECEIVER_LOG}" | tail -1)
  if [ -n "${decoder_line}" ]; then
    decoded=$(sed -n 's/.*decoded=\([0-9]*\).*/\1/p' <<<"${decoder_line}")
    in_kib=$(sed -n 's/.*chunks\/\([0-9]*\) KiB.*/\1/p' <<<"${decoder_line}")
    decoder_elem=$(sed -n 's/.*dec=\([a-zA-Z0-9_]*\).*/\1/p' <<<"${decoder_line}")
    decoder_state=$(sed -n 's/.*state=\([A-Z]*\).*/\1/p' <<<"${decoder_line}")
    decoder_fallback=$(sed -n 's/.*fallback=\([0-9]*\).*/\1/p' <<<"${decoder_line}")
  fi
  stats=$(curl -s -H "${AUTH_HEADER}" \
    "http://127.0.0.1:${CTRL_PORT}/stats" 2>/dev/null)
  [ "$(jq -r '.outputs[0].state // ""' <<<"${stats}" 2>/dev/null)" = "running" ] && out_running=1
  sent=$(jq -r '.outputs[0].bytes_sent // 0' <<<"${stats}" 2>/dev/null)
  pushed=$(jq -r '.outputs[0].bytes_pushed // 0' <<<"${stats}" 2>/dev/null)
  [[ "${sent:-0}" =~ ^[0-9]+$ ]] && out_bytes_sent=${sent}
  [[ "${pushed:-0}" =~ ^[0-9]+$ ]] && out_bytes_pushed=${pushed}
  [ "${out_running}" -eq 1 ] || \
    grep -aq "\[output local-rtmp\] running" "${RECEIVER_LOG}" && out_running=1
  sink=$(curl -s "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" 2>/dev/null)
  bytes=$(jq -r '.items[0].bytesReceived // 0' <<<"${sink}" 2>/dev/null)
  [[ "${bytes:-0}" =~ ^[0-9]+$ ]] && [ "${bytes}" -gt 0 ] && sink_bytes=${bytes}

  kill -0 "${feed_pid}" 2>/dev/null || feed_done=1

  if [ "${decoded:-0}" -gt 0 ] && [ "${decoder_state}" = "PLAYING" ] \
     && [ "${decoder_elem}" = "vaav1dec" ] && [ "${decoder_fallback:-1}" -eq 0 ] \
     && [ "${out_running}" -eq 1 ] && [ "${sink_bytes}" -gt 0 ] \
     && [ "${feed_done:-0}" -eq 1 ] && [ "${in_kib:-0}" -ge $(( fixture_kib - 8 )) ]; then
    break
  fi
  sleep 0.2
done

wait "${feed_pid}" 2>/dev/null || true
feed_pid=""

# ---- publish accounting ----------------------------------------------------
# The receiver's publish figure is measured at the destination element's sink
# pad (FLV bytes handed to rtmp2sink); MediaMTX counts the RTMP stream bytes it
# received, so that side also carries chunk headers and the handshake. The
# factors must agree to within the framing overhead; a wide gap means the
# receiver is counting something other than what left the pipeline.
sleep 2
final_stats=$(curl -s -H "${AUTH_HEADER}" \
  "http://127.0.0.1:${CTRL_PORT}/stats" 2>/dev/null)
pub_bytes=$(jq -r '.outputs[0].bytes_published_total // 0' <<<"${final_stats}" 2>/dev/null)
alias_bytes=$(jq -r '.outputs[0].bytes_sent // 0' <<<"${final_stats}" 2>/dev/null)
pub_frames=$(jq -r '.outputs[0].frames_published_total // 0' <<<"${final_stats}" 2>/dev/null)
rist_recv_total=$(jq -r '.rist.received_total // 0' <<<"${final_stats}" 2>/dev/null)
rist_recv_window=$(jq -r '.rist.received_window // 0' <<<"${final_stats}" 2>/dev/null)
rist_lost_total=$(jq -r '.rist.lost_total // 0' <<<"${final_stats}" 2>/dev/null)
rist_lost_window=$(jq -r '.rist.lost_window // 0' <<<"${final_stats}" 2>/dev/null)
sink=$(curl -s "http://127.0.0.1:${MTX_API_PORT}/v3/paths/list" 2>/dev/null)
bytes=$(jq -r '.items[0].bytesReceived // 0' <<<"${sink}" 2>/dev/null)
[[ "${bytes:-0}" =~ ^[0-9]+$ ]] && [ "${bytes}" -gt 0 ] && sink_bytes=${bytes}
ratio=$(awk -v p="${pub_bytes:-0}" -v s="${sink_bytes}" \
  'BEGIN { if (s > 0) printf "%.4f", p / s; else print "0" }')
# The figure the old counter reported for this same run (raw decoded frame
# bytes plus ingest bytes), kept as the measured contrast.
old_formula=$(( ${decoded:-0} * FIXTURE_WIDTH * FIXTURE_HEIGHT * 3 / 2 + ${in_kib:-0} * 1024 ))

echo
echo "=== decoded ==="
echo "${decoder_line:-<no [decoder] counters line>}"
echo
echo "=== published ==="
echo "[output local-rtmp] running=${out_running} sink_bytesReceived=${sink_bytes}"
echo "bytes_published_total=${pub_bytes:-0} frames_published_total=${pub_frames:-0} bytes_sent=${alias_bytes:-0}"
echo "published/sink=${ratio}"
echo "rist received_total=${rist_recv_total:-0} received_window=${rist_recv_window:-0} lost_total=${rist_lost_total:-0} lost_window=${rist_lost_window:-0}"
echo "old formula (decoded raw frames + ingest) would have reported ${old_formula} bytes = $(awk -v o="${old_formula}" -v s="${sink_bytes}" 'BEGIN { if (s > 0) printf "%.2fx", o / s; else print "n/a" }') the sink"
echo "[output local-rtmp] bytes_sent=${out_bytes_sent} bytes_pushed=${out_bytes_pushed}"

# bytes_sent is the bytes the sink took on its sink pad; the socket's own
# kernel counter (bytes_acked, which includes RTMP chunk and TCP/IP framing on
# top of that payload) is the independent truth it has to track. No absolute
# number: the assertion is the ratio between the two counters.
socket_bytes=0
if [ "${out_bytes_sent}" -gt 0 ]; then
  socket_bytes=$(ss -tin dst "${SINK_HOST}:${RTMP_PORT}" 2>/dev/null \
    | grep -o 'bytes_acked:[0-9]*' | cut -d: -f2 | awk '{s += $1} END {print s + 0}')
  [[ "${socket_bytes:-0}" =~ ^[0-9]+$ ]] || socket_bytes=0
fi
echo "[output local-rtmp] socket bytes_acked=${socket_bytes}"

fail=0
if [ -z "${decoder_line}" ]; then
  echo "FAIL: the decoder never logged counters; see ${RECEIVER_LOG}" >&2
  fail=1
elif [ "${decoder_state}" != "PLAYING" ]; then
  echo "FAIL: the decoder did not reach PLAYING (state=${decoder_state:-?})" >&2
  fail=1
elif [ "${decoded:-0}" -eq 0 ]; then
  echo "FAIL: the decoder decoded 0 frames" >&2
  fail=1
elif [ "${decoder_elem}" != "vaav1dec" ]; then
  echo "FAIL: decoded with ${decoder_elem:-no element}, not the VA-API vaav1dec" >&2
  fail=1
elif [ "${decoder_fallback:-1}" -ne 0 ]; then
  echo "FAIL: the decoder was on its software fallback (fallback=${decoder_fallback})" >&2
  fail=1
fi
if [ "${out_running}" -ne 1 ]; then
  echo "FAIL: the output never reported running" >&2
  fail=1
fi
if [ "${sink_bytes}" -eq 0 ]; then
  echo "FAIL: the RTMP sink received no bytes" >&2
  fail=1
fi
if [ "${pub_bytes:-0}" -eq 0 ]; then
  echo "FAIL: /stats reports bytes_published_total=0 after publishing" >&2
  fail=1
fi
if [ "${pub_frames:-0}" -eq 0 ]; then
  echo "FAIL: /stats reports frames_published_total=0 after publishing" >&2
  fail=1
fi
if [ "${alias_bytes:-0}" != "${pub_bytes:-0}" ]; then
  echo "FAIL: bytes_sent (${alias_bytes:-0}) and bytes_published_total (${pub_bytes:-0}) are not the same sink-pad measure" >&2
  fail=1
fi
if ! awk -v r="${ratio}" 'BEGIN { exit !(r >= 0.7 && r <= 1.3) }'; then
  echo "FAIL: bytes_published_total/sink_bytesReceived=${ratio} is outside 0.7-1.3" >&2
  fail=1
fi
# The cumulative figure covers every window, so it can never be smaller than the
# last one; if it is, the two are not being accumulated independently.
if [ "${rist_recv_total:-0}" -lt "${rist_recv_window:-0}" ]; then
  echo "FAIL: rist.received_total (${rist_recv_total:-0}) is below received_window (${rist_recv_window:-0})" >&2
  fail=1
fi
if [ "${rist_lost_total:-0}" -lt "${rist_lost_window:-0}" ]; then
  echo "FAIL: rist.lost_total (${rist_lost_total:-0}) is below lost_window (${rist_lost_window:-0})" >&2
  fail=1
fi
# bytes_sent must be socket egress, not raw decode nor ingest volume: it tracks
# the kernel's own byte count for the publish socket, and it is the smaller of
# the two appsrc/sink counters (raw decoded frames dominate a transcode feed).
if [ "${socket_bytes}" -eq 0 ]; then
  echo "FAIL: no bytes_acked for receiver -> ${SINK_HOST}:${RTMP_PORT} (cannot verify bytes_sent)" >&2
  fail=1
elif ! awk -v sent="${out_bytes_sent}" -v sock="${socket_bytes}" \
    'BEGIN { exit !(sent >= sock * 0.90 && sent <= sock * 1.02) }'; then
  echo "FAIL: bytes_sent=${out_bytes_sent} does not track the publish socket's ${socket_bytes} bytes_acked" >&2
  fail=1
fi
if [ "${out_bytes_pushed}" -le "${out_bytes_sent}" ]; then
  echo "FAIL: bytes_pushed=${out_bytes_pushed} is not above socket egress=${out_bytes_sent} on a transcode output" >&2
  fail=1
fi

if [ "${fail}" -ne 0 ]; then
  echo
  echo "=== receiver log (tail) ===" >&2
  tail -40 "${RECEIVER_LOG}" >&2
  echo "logs: ${RECEIVER_LOG} ${SINK_LOG}" >&2
  exit 1
fi

echo "GUARD OK: ${decoded} frames decoded by ${decoder_elem}, output running, sink received ${sink_bytes} bytes, /stats bytes_published_total=${pub_bytes:-0} (${ratio}x the sink; the residual is rtmp2sink's not-yet-flushed buffer and the sink's own framing), bytes_sent ${out_bytes_sent} tracked ${socket_bytes} socket bytes"
exit 0
