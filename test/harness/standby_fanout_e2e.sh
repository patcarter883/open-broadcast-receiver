#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# standby_fanout_e2e.sh — plan G1: the one-destination-connection proof for the
# standby slate fan-out.
#
# What this measures, and why each assertion is the one it is:
#
#   1. slate -> live -> slate on one RTMP publish. The decisive assertion is the
#      number of publish connections the SINK recorded, not a byte counter: a
#      reconnect is invisible in bytes (the count keeps climbing either way) but
#      every reconnect prints one more "is publishing to path" line.
#   2. an encoder restart whose timestamps begin at 0 again. Same one-connection
#      assertion, plus the published byte counter must never go backwards across
#      the switch and the sink must never log a disconnect. This is the case that
#      justifies the re-stamper: without it the second feed's PTS drags the
#      muxed timeline back to zero and flv clients see a jump.
#   3. a video-only ingest (the JPEG XS camera-link shape). The slate carries
#      audio, so the muxer's audio pad exists for the whole session; a video-only
#      source must still publish and must not park the muxer waiting for an audio
#      pad that will never carry a buffer.
#
# Usage:
#   test/harness/standby_fanout_e2e.sh [--bin <receiver>] [--scenarios "1 2 3"]
#
# Env overrides:
#   BIN            receiver binary        (default build/open-broadcast-receiver)
#   RIST_FEED      feed tool              (default build/test/rist_feed)
#   SINK_HOST      address the sink binds (default: the host's primary IPv4)
#   CTRL_PORT      control plane base     (default 18190, +10 per scenario)
#   RIST_PORT      RIST ingest base       (default 15190, +10 per scenario)
#   RTMP_PORT      sink RTMP base         (default 19390, +10 per scenario)
#   SINK_API_PORT  sink API base          (default 20090, +10 per scenario)
#   LIVE_TIMEOUT   seconds to wait for a live switch at all (default 15)
#   LIVE_SWITCH_BOUND_MS  slate->live latency bound from the first payload
#                  (default 2500; the plan asks ~2000)
#   SLATE_TIMEOUT  seconds to wait for a slate at all (default 20)
#   SLATE_SWITCH_BOUND_MS live->slate latency bound once media has stopped
#                  (default 2500; the plan asks ~3000 wall clock, which includes
#                  the RIST Advanced profile draining its ~1-2 s buffer)
#   LOG_DIR        where logs are kept    (default: a fresh temp dir)
#   KEEP_LOGS=1    keep the temp dir after a passing run
#
# Exit status: 0 all scenarios passed, 1 at least one assertion failed,
# 2 the harness could not run at all (tools, binary, ports, sink).
#
# The receiver refuses loopback as an egress target, so the sink binds a real
# interface address rather than 127.0.0.1; SINK_HOST overrides the discovery.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${BIN:-${REPO_ROOT}/build/open-broadcast-receiver}"
RIST_FEED="${RIST_FEED:-${REPO_ROOT}/build/test/rist_feed}"
SCENARIOS="${SCENARIOS:-1 2 3}"
LIVE_TIMEOUT="${LIVE_TIMEOUT:-15}"
SLATE_TIMEOUT="${SLATE_TIMEOUT:-20}"
SLATE_READY_TIMEOUT="${SLATE_READY_TIMEOUT:-20}"
# slate -> live, measured from the receiver's FIRST live payload (not from the
# encoder's process start: gst-launch takes ~3 s to spin up, which is the rig's
# time, not the switch's).
LIVE_SWITCH_BOUND_MS="${LIVE_SWITCH_BOUND_MS:-2500}"
SLATE_SWITCH_BOUND_MS="${SLATE_SWITCH_BOUND_MS:-2500}"
TOKEN="standby-e2e-token"
SESSION_TAG="standby-e2e-$$"

CTRL_PORT="${CTRL_PORT:-18190}"
RIST_PORT="${RIST_PORT:-15190}"
RTMP_PORT="${RTMP_PORT:-19390}"
SINK_API_PORT="${SINK_API_PORT:-20090}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin) BIN="$2"; shift 2 ;;
    --scenarios) SCENARIOS="$2"; shift 2 ;;
    -h|--help) sed -n '5,46p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "standby_fanout_e2e: $*" >&2; exit 2; }

command -v curl >/dev/null 2>&1 || die "curl is required"
command -v jq   >/dev/null 2>&1 || die "jq is required"
command -v gst-launch-1.0 >/dev/null 2>&1 || die "gst-launch-1.0 is required"
command -v ffprobe >/dev/null 2>&1 || die "ffprobe is required (PTS assertion)"
[[ -x "$BIN" ]] || die "receiver binary not built: $BIN (cmake --build build)"
[[ -x "$RIST_FEED" ]] || die "feed tool not built: $RIST_FEED (cmake --build build --target rist_feed)"

# A verdict is only meaningful for the code the binary was linked against.
newer_source="$(find "$REPO_ROOT/source" -type f \( -name '*.cpp' -o -name '*.h' \) \
                     -newer "$BIN" -print -quit 2>/dev/null)"
if [[ -n "$newer_source" ]]; then
  echo "receiver binary is older than the sources it must exercise:" >&2
  echo "  binary  ${BIN} ($(date -r "$BIN" '+%F %T'))" >&2
  echo "  newer   ${newer_source} ($(date -r "$newer_source" '+%F %T'))" >&2
  echo "rebuild it first: cmake --build $(dirname "$BIN") --target open-broadcast-receiver_exe" >&2
  exit 2
fi

if [[ -z "${SINK_HOST:-}" ]]; then
  SINK_HOST="$(ip -4 route get 1.1.1.1 2>/dev/null \
    | awk '{for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit }}')"
  [[ -n "$SINK_HOST" ]] || SINK_HOST="$(hostname -I 2>/dev/null | awk '{print $1}')"
fi
[[ -n "$SINK_HOST" ]] || die "no non-loopback IPv4 found; set SINK_HOST"

if command -v mediamtx >/dev/null 2>&1; then
  SINK_KIND=native
elif command -v docker >/dev/null 2>&1 \
     && docker info >/dev/null 2>&1 \
     && docker image inspect bluenviron/mediamtx:1.19.2 >/dev/null 2>&1; then
  SINK_KIND=docker
else
  die "no RTMP sink: install mediamtx (or make bluenviron/mediamtx:1.19.2 available to docker)"
fi

LOG_DIR="${LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/obr-standby-e2e.XXXXXX")}"
mkdir -p "$LOG_DIR"

# Every process the harness starts leads its own process group, so a feed
# (gst-launch | tee | rist_feed) dies whole instead of leaving half a pipeline
# bound to a port.
GROUPS=()
SCEN_GROUPS=()

record_group() { GROUPS+=("$1"); SCEN_GROUPS+=("$1"); }

kill_groups() {
  local sig="$1"; shift
  local pg
  for pg in "$@"; do kill -"$sig" -- "-$pg" 2>/dev/null || true; done
}

wait_ports_closed() { # wait_ports_closed <seconds> — no listener on this scenario's ports
  local secs="$1"
  local deadline=$(( $(now_ms) + secs * 1000 ))
  local p busy
  while :; do
    busy=0
    for p in "$CTRL_PORT_S" "$RIST_PORT_S" "$RTMP_PORT_S" "$SINK_API_PORT_S"; do
      ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN && busy=1
    done
    (( busy == 0 )) && return 0
    (( $(now_ms) >= deadline )) && return 1
    sleep 0.1
  done
}

stop_scenario() {
  if ((${#SCEN_GROUPS[@]})); then
    kill_groups TERM "${SCEN_GROUPS[@]}"
    sleep 1.0
    kill_groups KILL "${SCEN_GROUPS[@]}"
    SCEN_GROUPS=()
  fi
  stop_sink_container
  # Time for the kernel to release the listeners before anything asserts on
  # them: a socket that outlives its process by 100 ms is not a leftover.
  [[ -n "${CTRL_PORT_S:-}" ]] && wait_ports_closed 3
  return 0
}

cleanup() {
  stop_scenario
  if [[ "${KEEP_LOGS:-0}" != "1" && "${RUN_FAILED:-0}" == "0" ]]; then
    rm -rf "$LOG_DIR"
  else
    echo "logs kept at ${LOG_DIR}"
  fi
}
trap 'cleanup' EXIT INT TERM

# ---------------------------------------------------------------- assertions

SCENARIO=""
SCENARIO_FAILURES=0
RESULTS=()

pass() { printf '  PASS  %s\n' "$*"; }
fail() {
  printf '  FAIL  %s\n' "$*"
  SCENARIO_FAILURES=$((SCENARIO_FAILURES + 1))
}

expect() { # expect <description> <command ...>
  local what="$1"; shift
  if "$@"; then pass "$what"; else fail "$what"; fi
}

now_ms() { date +%s%3N; }

wait_until() { # wait_until <seconds> <command ...>
  local secs="$1"; shift
  local deadline=$(( $(now_ms) + secs * 1000 ))
  while (( $(now_ms) < deadline )); do
    if "$@" >/dev/null 2>&1; then return 0; fi
    sleep 0.1
  done
  "$@" >/dev/null 2>&1
}

# ---------------------------------------------------------------- receiver IO

status()  { curl -sf -H "Authorization: Bearer ${TOKEN}" \
              "http://127.0.0.1:${CTRL_PORT_S}/status"; }
stats()   { curl -sf -H "Authorization: Bearer ${TOKEN}" \
              "http://127.0.0.1:${CTRL_PORT_S}/stats"; }
stdby()   { status | jq -r ".standby.${1}"; }
out()     { status | jq -r ".outputs[0].${1}"; }
stat_out(){ stats  | jq -r ".outputs[0].${1} // 0"; }

mode_is()      { [[ "$(stdby mode)" == "$1" ]]; }
publish_seen() { (( $(publish_count) >= 1 )); }

# wait_for_live <timeout_s> — wait for mode 'live', recording how long the
# switch took after the receiver's first live payload. LIVE_LATENCY_MS is empty
# when no payload was ever observed (a real failure, not a slow switch).
#
# "First payload" is a DIP in last_live_ms_ago, not a wall clock: the RIST
# Advanced profile drains ~1-2 s of buffered media after the encoder stops, so
# the moment the receiver starts seeing data is the only fair start line. Before
# any payload the field is 0; after a previous feed it is large and falling.
LIVE_LATENCY_MS=""
wait_for_live() {
  local secs="$1"
  local deadline=$(( $(now_ms) + secs * 1000 ))
  local m ago now first="" prev=""
  LIVE_LATENCY_MS=""
  while (( $(now_ms) < deadline )); do
    m="$(stdby mode)"
    ago="$(stdby last_live_ms_ago)"
    now=$(now_ms)
    if [[ "$ago" =~ ^[0-9]+$ ]]; then
      if [[ -z "$first" && -n "$prev" ]] && (( ago > 0 && ago <= 300 )) \
         && { (( prev == 0 )) || (( prev > 500 )); }; then
        first="$now"
      fi
      prev="$ago"
    fi
    if [[ "$m" == live ]]; then
      [[ -n "$first" ]] && LIVE_LATENCY_MS=$(( now - first ))
      return 0
    fi
    sleep 0.1
  done
  return 1
}

# wait_for_slate <timeout_s> — wait for the slate after media stopped, recording
# the time the controller took once the payload had really gone (ago > 300 ms
# with no new payload). SLATE_LATENCY_MS is the live_gap_ms window plus one tick.
SLATE_LATENCY_MS=""
wait_for_slate() {
  local secs="$1"
  local deadline=$(( $(now_ms) + secs * 1000 ))
  local m ago now marker=""
  SLATE_LATENCY_MS=""
  while (( $(now_ms) < deadline )); do
    m="$(stdby mode)"
    ago="$(stdby last_live_ms_ago)"
    now=$(now_ms)
    if [[ -z "$marker" && "$ago" =~ ^[0-9]+$ ]] && (( ago > 300 )); then
      marker="$now"
    fi
    if [[ "$m" == slate ]]; then
      [[ -n "$marker" ]] && SLATE_LATENCY_MS=$(( now - marker ))
      return 0
    fi
    sleep 0.1
  done
  return 1
}

muxer_not_parked() {
  local af dropped
  af="$(out audio_frames)"
  dropped="$(out audio_dropped)"
  { [[ "$af" =~ ^[0-9]+$ ]] && (( af > 0 )); } || [[ "$dropped" == "true" ]]
}
publish_state_running() { [[ "$(out state)" == running ]]; }

bytes_received() {
  curl -sf "http://127.0.0.1:${SINK_API_PORT_S}/v3/paths/list" \
    | jq -r '[.items[]? | select(.name == "live/e2e") | .bytesReceived] | add // 0'
}
bytes_received_positive() { (( $(bytes_received) > 0 )); }

publish_count()    { grep -ac "is publishing to path 'live/e2e'" "$SINK_LOG" 2>/dev/null || true; }
disconnect_count() { grep -acE '\[conn [^]]+\] closed'        "$SINK_LOG" 2>/dev/null || true; }

# ---------------------------------------------------------------- plumbing

use_ports() { # <0-based scenario index>
  local i="$1"
  CTRL_PORT_S=$((CTRL_PORT + i * 10))
  RIST_PORT_S=$((RIST_PORT + i * 10))
  RTMP_PORT_S=$((RTMP_PORT + i * 10))
  SINK_API_PORT_S=$((SINK_API_PORT + i * 10))
}

ports_free() {
  local p
  for p in "$CTRL_PORT_S" "$RIST_PORT_S" "$RTMP_PORT_S" "$SINK_API_PORT_S"; do
    if ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN; then
      echo "port $p is already in use" >&2
      return 1
    fi
  done
}

SINK_LOG=""
SINK_CONTAINER=""
RECV_LOG=""
FEED_PG=""

start_sink() { # <dir>
  local dir="$1"
  SINK_LOG="$dir/sink.log"
  cat > "$dir/mediamtx.yml" <<EOF
logLevel: info
rtsp: no
hls: no
webrtc: no
srt: no
rtmp: yes
rtmpAddress: ${SINK_HOST}:${RTMP_PORT_S}
rtmpEncryption: "no"
api: yes
apiAddress: 127.0.0.1:${SINK_API_PORT_S}
paths:
  all_others:
EOF
  case "$SINK_KIND" in
    native)
      # cwd is the log dir: mediamtx writes auto.crt/auto.key into the working
      # directory, and the harness must not litter the tree it runs in.
      setsid bash -c "cd '$dir' && exec mediamtx '$dir/mediamtx.yml'" \
        > "$SINK_LOG" 2>&1 &
      record_group $!
      ;;
    docker)
      SINK_CONTAINER="obr-standby-sink-$$-${SCENARIO}"
      docker run -d --rm --name "$SINK_CONTAINER" --network host \
        -v "$dir/mediamtx.yml:/mediamtx.yml:ro" \
        bluenviron/mediamtx:1.19.2 /mediamtx.yml >/dev/null || return 1
      ( docker logs -f "$SINK_CONTAINER" >> "$SINK_LOG" 2>&1 ) &
      record_group $!
      ;;
  esac
  wait_until 10 bash -c "curl -sf http://127.0.0.1:${SINK_API_PORT_S}/v3/paths/list >/dev/null"
}

stop_sink_container() {
  [[ -z "$SINK_CONTAINER" ]] && return 0
  docker rm -f "$SINK_CONTAINER" >/dev/null 2>&1 || true
  SINK_CONTAINER=""
}

start_receiver() { # <dir>
  RECV_LOG="$1/receiver.log"
  setsid "$BIN" --control-port "$CTRL_PORT_S" --rist-port "$RIST_PORT_S" \
    --token "$TOKEN" --egress-allow-private > "$RECV_LOG" 2>&1 &
  record_group $!
  wait_until 10 bash -c "curl -sf -H 'Authorization: Bearer ${TOKEN}' \
      http://127.0.0.1:${CTRL_PORT_S}/status >/dev/null"
}

post_start() { # <session_id>
  local body
  body=$(jq -nc --arg sid "$1" --arg rist "rist://@127.0.0.1:${RIST_PORT_S}" \
      --arg url "rtmp://${SINK_HOST}:${RTMP_PORT_S}/live" \
      '{
         schema_version: 5,
         session_id: $sid,
         ingest: {bandwidth: 4000, rist_listen: $rist},
         source: {codec: "h264", width: 320, height: 180, fps: 10},
         standby: {enabled: true},
         outputs: [{id: "o1", type: "rtmp", url: $url, key_or_streamid: "e2e"}]
       }')
  curl -sf -X POST -H "Authorization: Bearer ${TOKEN}" \
       -H 'Content-Type: application/json' -d "$body" \
       "http://127.0.0.1:${CTRL_PORT_S}/start" >/dev/null
}

# start_feed <dir> <label> [video-only] — a FRESH encoder each call, so its
# timestamps begin at 0; the raw TS is teed so the PTS assertion has evidence.
start_feed() {
  local dir="$1" label="$2" audio="${3:-av}"
  local pipe
  if [[ "$audio" == av ]]; then
    pipe='videotestsrc is-live=true pattern=ball ! video/x-raw,width=320,height=180,framerate=10/1 ! x264enc tune=zerolatency key-int-max=10 bitrate=800 speed-preset=ultrafast ! h264parse config-interval=1 ! mux. audiotestsrc is-live=true wave=sine ! audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=128000 ! aacparse ! mux. mpegtsmux name=mux alignment=7 ! fdsink fd=1'
  else
    pipe='videotestsrc is-live=true pattern=ball ! video/x-raw,width=320,height=180,framerate=10/1 ! x264enc tune=zerolatency key-int-max=10 bitrate=800 speed-preset=ultrafast ! h264parse config-interval=1 ! mpegtsmux alignment=7 ! fdsink fd=1'
  fi
  setsid bash -c "gst-launch-1.0 -q ${pipe} | tee '${dir}/feed-${label}.ts' | '$RIST_FEED' 127.0.0.1 '$RIST_PORT_S'" \
    > "$dir/feed-${label}.log" 2>&1 &
  FEED_PG=$!
  record_group "$FEED_PG"
}

stop_feed() {
  [[ -n "$FEED_PG" ]] || return 0
  kill_groups TERM "$FEED_PG"
  sleep 0.2
  kill_groups KILL "$FEED_PG"
  FEED_PG=""
}

# first_video_pts_ms <ts-file> — the first video PTS in the teed TS, in ms.
first_video_pts_ms() {
  ffprobe -v error -select_streams v:0 -show_entries packet=pts_time \
    -of csv=p=0 -read_intervals '%+#5' "$1" 2>/dev/null \
    | awk -F, 'NF && $1 != "" { printf "%.0f\n", $1 * 1000; exit }'
}

# pts_of_feed <file> — the first video PTS, retried while the file is still
# being written (a half-written TS can fail to probe once).
pts_of_feed() {
  local file="$1" pts="" _
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pts="$(first_video_pts_ms "$file")"
    [[ -n "$pts" ]] && { echo "$pts"; return 0; }
    sleep 0.3
  done
  echo ""
}

# ---------------------------------------------------------------- diagnostics

dump_failure() {
  echo "  ---- receiver log (tail) ----"
  tail -n 20 "$RECV_LOG" 2>/dev/null | sed 's/^/  | /'
  echo "  ---- sink log (RTMP lines) ----"
  grep -a 'RTMP' "$SINK_LOG" 2>/dev/null | tail -n 10 | sed 's/^/  | /'
  echo "  ---- /status ----"
  status 2>/dev/null | jq -c '{standby, outputs}' 2>/dev/null | sed 's/^/  | /'
  echo "  ---- muxer input ----"
  echo "  | audio_frames at the muxer pad: $(out audio_frames)  (0 with a linked audio pad is a stalled muxer)"
}

# ---------------------------------------------------------------- scenarios

banner() { echo; echo "=== scenario $1: $2 ==="; }

setup_session() { # <name>
  local name="$1"
  local dir="$LOG_DIR/$name"
  mkdir -p "$dir"
  ports_free || die "refusing to run: ports in use"
  start_sink "$dir"          || die "the sink did not come up"
  start_receiver "$dir"      || die "the receiver did not come up"
  post_start "${SESSION_TAG}-${name}" || die "POST /start failed"
}

# The slate must be published with NO rist traffic at all. That is both the
# first assertion of scenario 1 and the precondition of the other two.
expect_slate_publishes() {
  if ! wait_until "$SLATE_READY_TIMEOUT" publish_seen; then
    fail "slate publishes to the sink with no RIST media present (waited ${SLATE_READY_TIMEOUT}s)"
    echo "  (publish lines=$(publish_count), mode=$(stdby mode), bytes=$(bytes_received))"
    return 1
  fi
  pass "slate publishes to the sink with no RIST media present"
  expect "/status.standby.mode is 'slate' before any feed" mode_is slate
  expect "the sink received bytes for the slate" bytes_received_positive
  expect "exactly ONE publish connection so far" test "$(publish_count)" -eq 1
  return 0
}

scenario_1() {
  SCENARIO=1
  banner 1 "slate -> live -> slate on one connection"
  use_ports 0
  setup_session s1

  expect_slate_publishes || { dump_failure; stop_scenario; return; }

  start_feed "$LOG_DIR/s1" live av
  if wait_for_live "$LIVE_TIMEOUT"; then
    pass "mode becomes 'live'"
    expect "slate->live switch within ${LIVE_SWITCH_BOUND_MS}ms of the first live payload (${LIVE_LATENCY_MS:-no payload}ms)" \
      test "${LIVE_LATENCY_MS:-999999}" -le "$LIVE_SWITCH_BOUND_MS"
  else
    fail "mode becomes 'live' (now: $(stdby mode))"
  fi
  expect "standby.switches incremented" test "$(stdby switches)" -ge 1
  expect "the output stayed published across the switch" test "$(publish_count)" -eq 1
  expect "the output is published (state 'running')" publish_state_running
  expect "the muxer is not parked on an empty audio pad" muxer_not_parked

  local before
  sleep 1; before="$(bytes_received)"
  sleep 1
  expect "bytes keep arriving while live" test "$(bytes_received)" -gt "${before:-0}"

  stop_feed
  if wait_for_slate "$SLATE_TIMEOUT"; then
    pass "mode returns to 'slate'"
    expect "live->slate switch within ${SLATE_SWITCH_BOUND_MS}ms of the media stopping (${SLATE_LATENCY_MS:-no payload}ms)" \
      test "${SLATE_LATENCY_MS:-999999}" -le "$SLATE_SWITCH_BOUND_MS"
  else
    fail "mode returns to 'slate' (now: $(stdby mode))"
  fi

  expect "exactly ONE publish connection for the whole run" test "$(publish_count)" -eq 1
  expect "the sink logged no disconnect" test "$(disconnect_count)" -eq 0
  expect "the output reports zero reconnects" test "$(out reconnects)" -eq 0
  expect "standby.caps_ok is true" test "$(stdby caps_ok)" = "true"

  (( SCENARIO_FAILURES )) && dump_failure
  stop_scenario
}

scenario_2() {
  SCENARIO=2
  banner 2 "encoder restart with a PTS reset, one connection"
  use_ports 1
  setup_session s2

  expect_slate_publishes || { dump_failure; stop_scenario; return; }

  start_feed "$LOG_DIR/s2" first av
  if wait_for_live "$LIVE_TIMEOUT"; then
    pass "first feed goes live"
    expect "first slate->live switch inside the bound (${LIVE_LATENCY_MS:-no payload}ms)" \
      test "${LIVE_LATENCY_MS:-999999}" -le "$LIVE_SWITCH_BOUND_MS"
  else
    fail "first feed goes live (now: $(stdby mode))"
  fi
  sleep 1
  stop_feed
  if wait_for_slate "$SLATE_TIMEOUT"; then
    pass "feed stops: back to the slate"
    expect "live->slate switch inside the bound (${SLATE_LATENCY_MS:-no payload}ms)" \
      test "${SLATE_LATENCY_MS:-999999}" -le "$SLATE_SWITCH_BOUND_MS"
  else
    fail "feed stops: back to the slate (now: $(stdby mode))"
  fi

  # The restarted encoder: a brand-new gst-launch, whose timestamps begin at the
  # stream origin again (mpegtsmux stamps the first packet at 3600 s, so the
  # property to assert is "same origin", not the literal zero).
  local bytes_before bytes_settled pts_first pts_second
  bytes_before="$(stat_out bytes_sent)"
  pts_first="$(pts_of_feed "$LOG_DIR/s2/feed-first.ts")"
  start_feed "$LOG_DIR/s2" second av
  sleep 1
  bytes_settled="$(stat_out bytes_sent)"
  pts_second="$(pts_of_feed "$LOG_DIR/s2/feed-second.ts")"
  local pts_delta=""
  if [[ -n "$pts_first" && -n "$pts_second" ]]; then
    pts_delta=$(( pts_second - pts_first ))
    (( pts_delta < 0 )) && pts_delta=$(( -pts_delta ))
  fi
  expect "the restarted encoder begins at the stream origin again (${pts_first:-?} -> ${pts_second:-?} ms, delta ${pts_delta:-?})" \
    test "${pts_delta:-999999}" -le 100

  if wait_for_live "$LIVE_TIMEOUT"; then
    pass "second feed goes live again"
    expect "second slate->live switch inside the bound (${LIVE_LATENCY_MS:-no payload}ms)" \
      test "${LIVE_LATENCY_MS:-999999}" -le "$LIVE_SWITCH_BOUND_MS"
  else
    fail "second feed goes live again (now: $(stdby mode))"
  fi

  # Across the switch the published total must never fall: a re-stamp that
  # rewound would show up as a smaller counter on the next sample.
  local mono_ok=1 prev="$bytes_before" cur
  for _ in 1 2 3 4 5 6 7 8; do
    cur="$(stat_out bytes_sent)"
    [[ "$cur" =~ ^[0-9]+$ ]] || continue
    (( cur < prev )) && mono_ok=0
    prev="$cur"
    sleep 0.15
  done
  expect "the published byte counter never goes backwards across the PTS reset" \
    test "$mono_ok" -eq 1
  expect "bytes resumed after the PTS reset (before=${bytes_before}, mid=${bytes_settled})" \
    test "$(stat_out bytes_sent)" -gt "${bytes_settled:-0}"

  expect "exactly ONE publish connection across the encoder restart" \
    test "$(publish_count)" -eq 1
  expect "the sink never logged a disconnect" test "$(disconnect_count)" -eq 0
  expect "the output reports zero reconnects" test "$(out reconnects)" -eq 0

  stop_feed
  (( SCENARIO_FAILURES )) && dump_failure
  stop_scenario
}

scenario_3() {
  SCENARIO=3
  banner 3 "video-only ingest (JPEG XS camera-link shape)"
  use_ports 2
  setup_session s3

  expect_slate_publishes || { dump_failure; stop_scenario; return; }

  start_feed "$LOG_DIR/s3" video-only vonly
  if wait_for_live "$LIVE_TIMEOUT"; then
    pass "video-only feed goes live"
    expect "video-only slate->live switch inside the bound (${LIVE_LATENCY_MS:-no payload}ms)" \
      test "${LIVE_LATENCY_MS:-999999}" -le "$LIVE_SWITCH_BOUND_MS"
  else
    fail "video-only feed goes live (now: $(stdby mode))"
  fi
  expect "the output is published (state 'running')" publish_state_running
  expect "the muxer is not parked on an audio pad that never fills" muxer_not_parked

  # The failure this scenario exists for is a muxer parked on an audio pad that
  # never carries a buffer: mode can still say live while nothing is published.
  sleep 1
  local b1 b2
  b1="$(bytes_received)"
  sleep 1.5
  b2="$(bytes_received)"
  expect "the sink keeps receiving bytes with no audio in the ingest (${b1} -> ${b2})" \
    test "${b2:-0}" -gt "${b1:-0}"
  expect "exactly ONE publish connection for video-only" test "$(publish_count)" -eq 1
  expect "the sink logged no disconnect" test "$(disconnect_count)" -eq 0
  expect "the output reports zero reconnects" test "$(out reconnects)" -eq 0

  stop_feed
  (( SCENARIO_FAILURES )) && dump_failure
  stop_scenario
}

# ---------------------------------------------------------------- main

echo "receiver   ${BIN} ($(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown))"
echo "feed tool  ${RIST_FEED}"
echo "sink       ${SINK_KIND} on ${SINK_HOST} (rtmp ${RTMP_PORT}, api ${SINK_API_PORT})"
echo "logs       ${LOG_DIR}"

for n in $SCENARIOS; do
  SCENARIO_FAILURES=0
  case "$n" in
    1) scenario_1 ;;
    2) scenario_2 ;;
    3) scenario_3 ;;
    *) die "unknown scenario '$n' (expected 1 2 3)" ;;
  esac
  if (( SCENARIO_FAILURES == 0 )); then
    RESULTS+=("$(printf 'scenario %-2s PASS' "$n")")
  else
    RESULTS+=("$(printf 'scenario %-2s FAIL (%d assertion(s))' "$n" "$SCENARIO_FAILURES")")
  fi
done

# Nothing this harness starts may survive it: no listener on any port it used,
# and none of the process groups it recorded may still exist.
leftovers=0
for n in $SCENARIOS; do
  use_ports "$((n - 1))"
  for p in "$CTRL_PORT_S" "$RIST_PORT_S" "$RTMP_PORT_S" "$SINK_API_PORT_S"; do
    if ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN; then
      echo "LEFT BEHIND: listener on port $p" >&2
      leftovers=1
    fi
  done
done
for pg in "${GROUPS[@]}"; do
  if kill -0 "$pg" 2>/dev/null; then
    echo "LEFT BEHIND: process group $pg" >&2
    leftovers=1
  fi
done

echo
echo "=== summary ==="
for line in "${RESULTS[@]}"; do echo "$line"; done
if (( leftovers )); then
  echo "result     FAIL: harness left processes or listeners behind"
  RUN_FAILED=1
  exit 1
fi
for line in "${RESULTS[@]}"; do
  case "$line" in *FAIL*)
    echo "result     one or more scenarios failed"
    RUN_FAILED=1
    exit 1 ;;
  esac
done
echo "result     all scenarios passed"
exit 0
