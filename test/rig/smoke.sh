#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Single-path real-media smoke (FIXPLAN M4.1). The fast, per-PR proof of the
# core product function: a synthetic H.264+AAC stream flows encoder-side →
# RIST → the hosted-profile receiver → RTMP → SRS, and the SRS stream is
# actually PLAYABLE (ffprobe sees h264+aac; ffmpeg decodes it). No bonding, no
# netem — that is the nightly rig.sh. Wall-clock ~30s after the image builds.
set -euo pipefail
cd "$(dirname "$0")"

COMPOSE="docker compose -f docker-compose.smoke.yml"
TOKEN=smoketoken

api()  { $COMPOSE exec -T receiver curl -s -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:8080$1"; }
post() { $COMPOSE exec -T receiver curl -s -X POST -H "Authorization: Bearer $TOKEN" -d "$2" "http://127.0.0.1:8080$1"; }
fail() { echo "SMOKE FAIL: $*" >&2; $COMPOSE logs --tail 80 receiver sender srs >&2 || true; $COMPOSE down -v || true; exit 1; }

echo "== build images =="
$COMPOSE build receiver sender prober

echo "== start receiver + srs =="
$COMPOSE up -d receiver srs
for _ in $(seq 30); do api /status >/dev/null 2>&1 && break; sleep 1; done
api /status >/dev/null 2>&1 || fail "receiver control plane never came up"

echo "== /start: one RTMP output to SRS =="
srs_ip=$($COMPOSE exec -T receiver getent hosts srs | awk '{print $1}')
[ -n "$srs_ip" ] || fail "could not resolve srs"
post /start "{\"schema_version\":2,\"session_id\":\"smoke-1\",\"ingest\":{\"bandwidth\":6000},\"source\":{\"codec\":\"h264\"},\"outputs\":[{\"id\":\"rtmp1\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs_ip}:1935/live\",\"key_or_streamid\":\"smoke\"}]}" \
  | grep -q '"ok":true' || fail "/start rejected"

echo "== start sender (RIST → receiver) =="
$COMPOSE up -d sender

echo "== let media flow to SRS =="
sleep 25

# Assertion 1 — the receiver ingested RIST and the RTMP output is running + sending.
api /stats | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert d['rist']['received']>0, 'no RIST bytes received'
o={x['id']:x for x in d['outputs']}['rtmp1']
assert o['state']=='running', 'rtmp output not running: '+o['state']
assert o['bytes_sent']>0, 'rtmp output sent no bytes'
print('receiver ok: rist.received',d['rist']['received'],'rtmp bytes_sent',o['bytes_sent'])
" || fail "receiver /stats assertions"

# Assertion 2 — the SRS RTMP stream is PLAYABLE: ffprobe sees h264 video + aac audio.
echo "== probe SRS RTMP (ffprobe: expect h264 + aac) =="
probe=$($COMPOSE run --rm -T prober \
          ffprobe -v error -show_entries stream=codec_type,codec_name -of json \
          "rtmp://${srs_ip}:1935/live/smoke") \
  || fail "ffprobe could not open the RTMP stream (not playable)"
echo "$probe" | python3 -c "
import json,sys
d=json.load(sys.stdin)
# FLV/RTMP can expose a data/undetermined track with a codec_type but no
# codec_name — read codec_name defensively so it doesn't KeyError before the
# video/audio assertions run.
codecs={s['codec_type']:s.get('codec_name') for s in d.get('streams',[])}
assert codecs.get('video')=='h264', 'video not h264: '+str(codecs)
assert codecs.get('audio')=='aac', 'audio not aac: '+str(codecs)
print('SRS stream playable:',codecs)
" || fail "SRS stream is not a valid H.264+AAC stream"

# Assertion 3 — it actually DECODES (real frames, not just a header): 3s to null sink.
echo "== decode 3s from SRS (ffmpeg -f null) =="
$COMPOSE run --rm -T prober \
  ffmpeg -v error -i "rtmp://${srs_ip}:1935/live/smoke" -t 3 -f null - \
  && echo "decode ok: 3s decoded cleanly" || fail "SRS stream did not decode"

post /stop '{}' | grep -q '"stopped"' || fail "/stop failed"
echo "== SMOKE PASS =="
$COMPOSE down -v
