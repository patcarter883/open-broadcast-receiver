#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# MT1.9 — the transcode tier's acceptance proof (FIXPLAN MT1.9 / AV1_TRANSCODE §5.2).
#
# One AV1 ingest over RIST, fanned out three ways AT ONCE:
#   rtmp1  AV1 -> H.264   (RTMP, legacy flvmux)            transcode
#   srt1   AV1 -> H.265   (SRT, mpegtsmux)                 transcode
#   srt2   AV1 -> AV1     (SRT, tsparse passthrough)       copy
#
# What this proves, and why each part matters:
#   1. A non-H.264 source re-encodes to BOTH targets concurrently. The tier
#      exists because an AV1 ingest cannot be copied to RTMP (GStreamer 1.28.6
#      has no AV1-in-FLV), so a transcode target is not optional there.
#   2. The SAME ingest is copied verbatim to a third output at the same time --
#      the copy path is a tsparse passthrough and must not be disturbed by the
#      re-encode paths.
#   3. An output that loses its sink does not disturb the others.
#   4. The outputs actually carry the CODECS they claim, checked on real bytes
#      pulled back off the wire, not on the receiver's own status.
set -euo pipefail
cd "$(dirname "$0")"

COMPOSE="docker compose -f docker-compose.transcode.yml"
TOKEN=smoketoken

api()  { $COMPOSE exec -T receiver curl -s -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:8080$1"; }
post() { $COMPOSE exec -T receiver curl -s -X POST -H "Authorization: Bearer $TOKEN" -d "$2" "http://127.0.0.1:8080$1"; }
fail() { echo "TRANSCODE FAIL: $*" >&2; $COMPOSE logs --tail 100 receiver sender srs srt-h265 srt-av1copy >&2 || true; $COMPOSE down -v || true; exit 1; }

echo "== build images =="
$COMPOSE build receiver sender prober srt-h265 srt-av1copy

echo "== start receiver + both sinks + srs =="
$COMPOSE up -d receiver srs srt-h265 srt-av1copy
for _ in $(seq 30); do api /status >/dev/null 2>&1 && break; sleep 1; done
api /status >/dev/null 2>&1 || fail "receiver control plane never came up"

srs_ip=$($COMPOSE exec -T receiver getent hosts srs | awk '{print $1}')
h265_ip=$($COMPOSE exec -T receiver getent hosts srt-h265 | awk '{print $1}')
av1_ip=$($COMPOSE exec -T receiver getent hosts srt-av1copy | awk '{print $1}')
[ -n "$srs_ip" ] && [ -n "$h265_ip" ] && [ -n "$av1_ip" ] || fail "could not resolve sinks"

echo "== /start: schema 3, AV1 source, three concurrent destinations =="
# Capture the body rather than piping it into grep: a rejection carries the
# server's OWN reason (invalid_schema / bad_enum / transcode_unavailable) and a
# bare "rejected" throws away the only evidence of what it objected to.
start_body="{\"schema_version\":3,\"session_id\":\"mt19-1\",\"ingest\":{\"bandwidth\":6000},\"source\":{\"codec\":\"av1\"},\"outputs\":["
start_body+="{\"id\":\"rtmp1\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs_ip}:1935/live\",\"key_or_streamid\":\"mt19\",\"transcode\":{\"codec\":\"h264\"}},"
start_body+="{\"id\":\"srt1\",\"type\":\"srt\",\"url\":\"srt://${h265_ip}:9000\",\"key_or_streamid\":\"\",\"transcode\":{\"codec\":\"h265\"}},"
start_body+="{\"id\":\"srt2\",\"type\":\"srt\",\"url\":\"srt://${av1_ip}:9001\",\"key_or_streamid\":\"\"}]}"
start_resp=$(post /start "$start_body")
echo "$start_resp" | grep -q '"ok":true' || fail "/start rejected: ${start_resp}"

echo "== start the AV1 sender (RIST -> receiver) =="
$COMPOSE up -d sender

echo "== let media flow =="
sleep 30

# ---- Claim 1 + 2: three outputs, concurrently, all moving -------------------
api /stats | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert d['rist']['received']>0, 'no RIST bytes received'
o={x['id']:x for x in d['outputs']}
for want in ('rtmp1','srt1','srt2'):
    assert want in o, 'missing output '+want
    assert o[want]['state']=='running', want+' not running: '+o[want]['state']
    assert o[want]['bytes_sent']>0, want+' sent no bytes'
print('3 concurrent outputs running:', {k:o[k]['bytes_sent'] for k in ('rtmp1','srt1','srt2')})
" || fail "concurrent output assertions"

# ---- Claim 4: the transcode outputs run on the GPU, not on the CPU -----------
# The first production use is a node on hardware, so "it transcoded" is not the
# bar: a silent fallback to x264enc/x265enc would pass a codec check and still be
# the wrong deployment. /status names the encoder actually chosen (MT1.8).
api /stats | python3 -c "
import json,sys
o={x['id']:x for x in json.load(sys.stdin)['outputs']}
got={k:(o[k].get('transcode') or {}).get('encoder') for k in ('rtmp1','srt1')}
print('transcode encoders chosen:', got)
assert got['rtmp1']=='vah264enc', 'rtmp1 did NOT use hardware H.264 encode -- got '+str(got['rtmp1'])+' (CPU fallback)'
assert got['srt1']=='vah265enc', 'srt1 did NOT use hardware H.265 encode -- got '+str(got['srt1'])+' (CPU fallback)'
assert (o['srt2'].get('transcode') or {}).get('encoder') in (None,''), 'srt2 is a COPY output but reports a transcoder'
print('hardware encode confirmed on both transcode outputs')
" || fail "transcode did not use the GPU"

# ---- Claim 5: the codecs on the wire are what each output claims -------------
echo "== probe rtmp1 at SRS (expect h264 video + aac audio) =="
$COMPOSE run --rm -T prober \
  ffprobe -v error -show_entries stream=codec_type,codec_name -of json \
  "rtmp://${srs_ip}:1935/live/mt19" > /tmp/mt19-rtmp.json \
  || fail "rtmp1 not playable at SRS"
python3 -c "
import json
d=json.load(open('/tmp/mt19-rtmp.json'))
c={s['codec_type']:s.get('codec_name') for s in d.get('streams',[])}
assert c.get('video')=='h264', 'rtmp1 video not h264 (+transcode): '+str(c)
print('rtmp1 ->', c)
" || fail "rtmp1 transcode target wrong"

echo "== probe srt1 (expect h265) and srt2 (expect av1) =="
$COMPOSE run --rm -T prober \
  ffprobe -v error -show_entries stream=codec_type,codec_name -of json /out265/h265.ts > /tmp/mt19-h265.json \
  || fail "srt1 produced no readable TS"
$COMPOSE run --rm -T prober \
  ffprobe -v error -show_entries stream=codec_type,codec_name -of json /outav1/av1.ts > /tmp/mt19-av1.json \
  || fail "srt2 produced no readable TS"
python3 -c "
import json
h=json.load(open('/tmp/mt19-h265.json'))
a=json.load(open('/tmp/mt19-av1.json'))
hc={s['codec_type']:s.get('codec_name') for s in h.get('streams',[])}
ac={s['codec_type']:s.get('codec_name') for s in a.get('streams',[])}
assert hc.get('video')=='h265', 'srt1 video not h265 (transcode): '+str(hc)
assert ac.get('video')=='av1', 'srt2 video is not the AV1 COPY -- it was re-encoded or dropped: '+str(ac)
print('srt1 ->', hc, ' srt2 ->', ac)
" || fail "transcode/copy targets wrong"

# ---- Claim 3: one output losing its sink must not disturb the others ---------
echo "== kill the RTMP sink mid-run: srt1/srt2 must keep flowing =="
before=$(api /stats | python3 -c "
import json,sys
o={x['id']:x for x in json.load(sys.stdin)['outputs']}
print(o['srt1']['bytes_sent'], o['srt2']['bytes_sent'])")
$COMPOSE stop srs >/dev/null 2>&1
sleep 20
after=$(api /stats | python3 -c "
import json,sys
d=json.load(sys.stdin)
o={x['id']:x for x in d['outputs']}
print(o['srt1']['bytes_sent'], o['srt2']['bytes_sent'])
import sys as s
assert o['srt1']['state']=='running' and o['srt2']['state']=='running', 'a copy/re-encode output was disturbed by another output losing its sink'
s.stderr.write('rtmp1 state with its sink down: '+o['rtmp1']['state']+'\n')")
b1=$(echo $before | cut -d' ' -f1); a1=$(echo $after | cut -d' ' -f1)
b2=$(echo $before | cut -d' ' -f2); a2=$(echo $after | cut -d' ' -f2)
[ "$a1" -gt "$b1" ] || fail "srt1 stopped sending while srs was down"
[ "$a2" -gt "$b2" ] || fail "srt2 stopped sending while srs was down"
echo "  srt1 $b1 -> $a1, srt2 $b2 -> $a2 (unaffected)"

echo "== bring the sink back: rtmp1 must recover =="
$COMPOSE start srs >/dev/null 2>&1
recovered=0
for _ in $(seq 30); do
  st=$(api /stats | python3 -c "
import json,sys
o={x['id']:x for x in json.load(sys.stdin)['outputs']}
print(o['rtmp1']['state'])" 2>/dev/null || echo "?")
  [ "$st" = "running" ] && { recovered=1; break; }
  sleep 2
done
[ "$recovered" = "1" ] || fail "rtmp1 did not recover after its sink returned"
echo "  rtmp1 recovered to running"

post /stop '{}' | grep -q '"stopped"' || fail "/stop failed"
echo "== TRANSCODE PASS: AV1 -> {H.264 RTMP + H.265 SRT + AV1 copy}, concurrent, and one output's failure did not disturb the others =="
$COMPOSE down -v
