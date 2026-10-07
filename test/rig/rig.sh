#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Bonding CI rig driver (FIXPLAN M1.6 + M1.8).
#
# Stages:
#   red-proof  — receiver built with timing-mode=1 (ARRIVAL) must CRASH under
#                the bonded double hop within RED_WINDOW seconds. Proves the
#                rig detects the original C1 defect before pinning mode 0.
#   soak       — timing-mode=0 run for RIG_DURATION seconds (nightly: 900).
#                Assertions: no abnormal container exit; TWO peers in /stats;
#                traffic shifts on a single-path outage; CGNAT-churn survival;
#                drop-storm recovery (M1.8) with dropped_bytes accounting;
#                outputs recover to running; recording grows.
#
# Env: RIG_DURATION (default 900), RED_WINDOW (default 120),
#      SKIP_RED=1 to skip the red-proof stage (local iteration).
set -euo pipefail
cd "$(dirname "$0")"

RIG_DURATION="${RIG_DURATION:-900}"
RED_WINDOW="${RED_WINDOW:-120}"
COMPOSE="docker compose -f docker-compose.yml"
CURL_R="docker compose -f docker-compose.yml exec -T receiver curl -s -H 'Authorization: Bearer rigtoken'"

api() { $COMPOSE exec -T receiver curl -s -H "Authorization: Bearer rigtoken" "http://127.0.0.1:8080$1" ; }
post() { $COMPOSE exec -T receiver curl -s -X POST -H "Authorization: Bearer rigtoken" -d "$2" "http://127.0.0.1:8080$1" ; }
py() { python3 -c "$1" ; }
fail() { echo "RIG FAIL: $*" >&2; $COMPOSE logs --tail 50 receiver bond sender >&2 || true; $COMPOSE down -v || true; exit 1; }

start_session() {
  local srs1_ip srs2_ip srt_ip
  srs1_ip=$($COMPOSE exec -T receiver getent hosts srs1 | awk '{print $1}')
  srs2_ip=$($COMPOSE exec -T receiver getent hosts srs2 | awk '{print $1}')
  srt_ip=$($COMPOSE exec -T receiver getent hosts srt-listener | awk '{print $1}')
  post /start "{\"schema_version\":4,\"session_id\":\"rig-1\",\"ingest\":{\"bandwidth\":6000},\"source\":{\"codec\":\"h264\"},\"outputs\":[
    {\"id\":\"rtmp1\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs1_ip}:1935/live\",\"key_or_streamid\":\"rig\"},
    {\"id\":\"rtmp2\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs2_ip}:1935/live\",\"key_or_streamid\":\"rig\"},
    {\"id\":\"srt\",\"type\":\"srt\",\"url\":\"srt://${srt_ip}:9000\",\"key_or_streamid\":\"\"}]}"
}

stat_field() { api /stats | py "import json,sys; d=json.load(sys.stdin); print($1)" ; }

# ---------------------------------------------------------------------------
# Stage 1: red-proof (the test must detect timing-mode=1)
# ---------------------------------------------------------------------------
if [ "${SKIP_RED:-0}" != "1" ]; then
  echo "== red-proof: building receiver with timing-mode=1 (must crash) =="
  RIG_TIMING_MODE=1 $COMPOSE build receiver
  RIG_TIMING_MODE=1 $COMPOSE up -d
  sleep 5
  start_session > /dev/null
  echo "waiting up to ${RED_WINDOW}s for the ARRIVAL-mode abort..."
  crashed=0
  for _ in $(seq "$RED_WINDOW"); do
    if ! $COMPOSE ps --services --filter status=running | grep -qx receiver; then
      crashed=1; break
    fi
    sleep 1
  done
  [ "$crashed" = "1" ] || fail "timing-mode=1 receiver did NOT crash — the rig cannot detect the C1 defect"
  echo "ok: ARRIVAL-mode receiver aborted as expected (rig detects the defect)"
  $COMPOSE down -v
fi

# ---------------------------------------------------------------------------
# Stage 2: soak with timing-mode=0
# ---------------------------------------------------------------------------
echo "== soak: timing-mode=0, ${RIG_DURATION}s =="
RIG_TIMING_MODE=0 $COMPOSE build receiver
RIG_TIMING_MODE=0 $COMPOSE up -d --build
sleep 8
start_session | grep -q '"ok":true' || fail "/start rejected"

echo "waiting for media + outputs..."
sleep 30
peers=$(stat_field "len(d['rist']['peers'])")
[ "$peers" -ge 2 ] || fail "expected >=2 RIST peers (bonded paths), got $peers"
echo "ok: $peers peers on one session port (H1 topology proof)"

deadline=$(( $(date +%s) + RIG_DURATION ))
mid_events_done=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  # continuous invariant: nothing exits abnormally (C1 regression)
  want="receiver bond sender srs1 srs2 srt-listener"
  for svc in $want; do
    # srs1 is legitimately paused during the drop-storm window
    $COMPOSE ps --services --filter status=running | grep -qx "$svc" \
      || $COMPOSE ps --services --filter status=paused | grep -qx "$svc" \
      || fail "container '$svc' left running state during soak"
  done

  # one-shot mid-soak fault injections, ~1/3 into the run
  if [ "$mid_events_done" = "0" ] \
     && [ "$(date +%s)" -gt $(( deadline - RIG_DURATION * 2 / 3 )) ]; then
    mid_events_done=1

    echo "-- fault: 10s full outage on wan_a (traffic must shift to peer B)"
    before_b=$(stat_field "d['rist']['peers'][1]['received_bytes'] if len(d['rist']['peers'])>1 else 0")
    $COMPOSE exec -T bond sh -c 'dev=$(ip -o route get 172.31.101.10 | sed -n "s/.* dev \([^ ]*\).*/\1/p"); tc qdisc replace dev $dev root netem loss 100%'
    sleep 10
    $COMPOSE exec -T bond sh -c 'dev=$(ip -o route get 172.31.101.10 | sed -n "s/.* dev \([^ ]*\).*/\1/p"); tc qdisc replace dev $dev root netem delay 40ms 10ms loss 2%'
    after_b=$(stat_field "d['rist']['peers'][1]['received_bytes'] if len(d['rist']['peers'])>1 else 0")
    [ "$after_b" -gt "$before_b" ] || fail "peer B carried no traffic during path-A outage"
    echo "ok: traffic shifted to surviving path"

    echo "-- fault: CGNAT churn — source rebinding on path A (H1 open question)"
    $COMPOSE exec -T bond sh -c 'conntrack -F 2>/dev/null || true'
    $COMPOSE restart bond > /dev/null   # hard rebind: new source ports on both paths
    sleep 15
    api /stats | grep -q '"received"' || fail "receiver unreachable after churn"
    echo "ok: flow survived source rebinding"

    echo "-- fault (M1.8): drop-storm — pause srs1 45s to force ring drops"
    $COMPOSE pause srs1
    sleep 45
    $COMPOSE unpause srs1
    sleep 30
    dropped=$(stat_field "[o for o in d['outputs'] if o['id']=='rtmp1'][0]['dropped_bytes']")
    state1=$(stat_field "[o for o in d['outputs'] if o['id']=='rtmp1'][0]['state']")
    echo "rtmp1 after storm: state=$state1 dropped_bytes=$dropped"
    # flvmux must have survived the forward timestamp jump: output back up
    [ "$state1" = "running" ] || fail "rtmp1 did not recover after drop storm"
    echo "ok: drop storm recovered (dropped_bytes=$dropped accounted)"
  fi
  sleep 5
done

# final assertions
api /stats | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert len(d['rist']['peers'])>=2, 'peers'
assert d['rist']['recovered']>0, 'netem loss must exercise recovery'
outs={o['id']:o for o in d['outputs']}
assert outs['srt']['state']=='running' and outs['srt']['dropped_bytes']==0, 'srt continuity'
assert outs['rtmp2']['state']=='running', 'rtmp2 continuity'
assert d['recording']['bytes']>0, 'recording grew'
print('final: peers',len(d['rist']['peers']),'recovered',d['rist']['recovered'],
      'srt sent',outs['srt']['bytes_sent'],'recording',d['recording']['bytes'])
" || fail "final stats assertions"

api /status | grep -q '"state":"running"' || fail "session not running at soak end"
post /stop '{}' | grep -q '"stopped"' || fail "/stop failed"
echo "== RIG PASS =="
$COMPOSE down -v
