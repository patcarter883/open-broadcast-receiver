#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# MT1.9 — the transcode tier's acceptance proof (FIXPLAN MT1.9 / AV1_TRANSCODE §5.2).
#
# One AV1 ingest over RIST, fanned out five ways AT ONCE:
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
#   rtmp2   AV1 -> H.264  1080p UPSCALED to 1440p (GPU)      transcode+scale
#   rtmp3   AV1 -> H.265  (RTMP, eflvmux/enhanced)            transcode
#   3. An output that loses its sink does not disturb the others.
#   4. The outputs actually carry the CODECS they claim, checked on real bytes
#   5. av1 on an RTMP destination is REFUSED at /start (the FLV muxers cannot
#      carry it), so the refusal is exercised, not just the happy path.
#   6. A scaled output is scaled -- checked on the bytes at the sink, since
#      /stats names the encoder and not the scaler.
#      pulled back off the wire, not on the receiver's own status.
set -euo pipefail
cd "$(dirname "$0")"

# Serialise rig runs. This script starts with `docker compose down -v` and brings up
# fixed container names and ports, so two concurrent runs tear each other's containers
# down mid-flow and produce bogus assertion failures -- which is exactly how a run's
# receiver got replaced underneath it and its decode-stage logs vanished. Refuse to
# start rather than collide.
exec 9>/tmp/obr-rig.lock
if ! flock -n 9; then
  echo "another rig run is in progress (holding /tmp/obr-rig.lock); refusing to collide" >&2
  exit 1
fi

COMPOSE="docker compose -f docker-compose.transcode.yml"
TOKEN=smoketoken

# Every call is bounded, because a wedged `docker compose exec` is indistinguishable
# from a slow stage and stalls the whole run (one cost ~10 minutes; the receiver was
# answering in 0.2ms the entire time, so the product was innocent).
#
# The two budgets differ on purpose. A probe (/status, /stats) should give up fast.
# /stop drains a live pipeline -- output threads, RIST peers, the recorder -- and
# legitimately takes tens of seconds, so a probe-sized budget fails a stop that is
# only slow, not broken.
api()  { $COMPOSE exec -T receiver curl -s -m 15 -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:8080$1"; }
post() { $COMPOSE exec -T receiver curl -s -m 90 -X POST -H "Authorization: Bearer $TOKEN" -d "$2" "http://127.0.0.1:8080$1"; }
# Dump enough to actually diagnose. --tail 100 across five services scrolls the
# receiver's EARLY errors (a failing output errors while it first connects) out of
# the window before it is read, which is how a real bus error got lost twice. Pull
# the receiver and srs in full, and surface the pipeline errors explicitly.
fail() {
  echo "TRANSCODE FAIL: $*" >&2
  echo "---- receiver (full) ----" >&2
  $COMPOSE logs --no-log-prefix --tail 400 receiver >&2 || true
  echo "---- receiver: errors only ----" >&2
  $COMPOSE logs receiver 2>&1 | grep -iE "error|warn|not-negotiated|refused|failed|reason|rtmp" >&2 || true
  echo "---- srs ----" >&2
  $COMPOSE logs --tail 80 srs >&2 || true
  echo "---- sender ----" >&2
  $COMPOSE logs --tail 40 sender >&2 || true
  echo "---- output state ----" >&2
  api /stats >&2 || true
  $COMPOSE down -v >/dev/null 2>&1 || true
  exit 1
}

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
start_body="{\"schema_version\":4,\"session_id\":\"mt19-1\",\"ingest\":{\"bandwidth\":6000},\"source\":{\"codec\":\"av1\"},\"outputs\":["
start_body+="{\"id\":\"rtmp1\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs_ip}:1935/live\",\"key_or_streamid\":\"mt19\",\"transcode\":{\"codec\":\"h264\",\"bitrate_kbps\":2500,\"gop\":60}},"
start_body+="{\"id\":\"srt1\",\"type\":\"srt\",\"url\":\"srt://${h265_ip}:9000\",\"key_or_streamid\":\"\",\"transcode\":{\"codec\":\"h265\",\"bitrate_kbps\":2500,\"gop\":60}},"
start_body+="{\"id\":\"srt2\",\"type\":\"srt\",\"url\":\"srt://${av1_ip}:9001\",\"key_or_streamid\":\"\"},"
# rtmp2 is the LADDER-HACK case: 1080p ingest upscaled to 1440p on the GPU and
# re-encoded. Nothing else proves the scaler ran -- /stats names the encoder, not the
# scaler -- so the resolution is checked on the bytes at the sink.
start_body+="{\"id\":\"rtmp2\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs_ip}:1935/live\",\"key_or_streamid\":\"scale\",\"transcode\":{\"codec\":\"h264\",\"bitrate_kbps\":8000,\"gop\":120,\"scale\":{\"width\":2560,\"height\":1440}}},"
# rtmp3 is H.265 over Enhanced RTMP, which only eflvmux can carry (flvmux is h264-only).
start_body+="{\"id\":\"rtmp3\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs_ip}:1935/live\",\"key_or_streamid\":\"h265\",\"transcode\":{\"codec\":\"h265\",\"bitrate_kbps\":8000,\"gop\":120}}]}"
# av1 rides in MPEG-TS only: flvmux/eflvmux have no video/x-av1 caps. The portal
# refuses it at write time, but the node must refuse it too, so a config arriving by
# any other route is not accepted and then left to fail negotiation.
#
# This MUST run before the real /start: afterwards the receiver answers
# already_running and never reaches target validation, so the assertion would pass
# for the wrong reason and would keep passing if the av1 gate were removed.
echo "== before any session: /start with av1 on rtmp must be REFUSED =="
bad_body="{\"schema_version\":4,\"session_id\":\"mt19-refusal\",\"ingest\":{\"bandwidth\":6000},\"source\":{\"codec\":\"av1\"},\"outputs\":[{\"id\":\"bad1\",\"type\":\"rtmp\",\"url\":\"rtmp://${srs_ip}:1935/live\",\"key_or_streamid\":\"bad\",\"transcode\":{\"codec\":\"av1\",\"bitrate_kbps\":2500,\"gop\":120}}]}"
bad_resp=$(post /start "$bad_body")
echo "$bad_resp" | grep -q '"ok":true' \
  && fail "av1 on an RTMP destination was ACCEPTED -- it cannot negotiate: ${bad_resp}"
echo "$bad_resp" | grep -q '"error_code":"bad_enum"' \
  || fail "av1-on-rtmp was not refused with bad_enum -- got: ${bad_resp}"
echo "  refused with bad_enum, as required"

start_resp=$(post /start "$start_body")
echo "$start_resp" | grep -q '"ok":true' || fail "/start rejected: ${start_resp}"

echo "== start the AV1 sender (RIST -> receiver) =="
$COMPOSE up -d sender

echo "== let media flow =="
sleep 30

# ---- Claim 1 + 2: three outputs, concurrently, all moving -------------------
# POLL, don't sample. An output reads "starting" while it connects to its sink, and
# an RTMP sink reconnects as a matter of course -- srs logs a normal
# unpublish/publish cycle. A single sample therefore fails a healthy output for
# being caught mid-reconnect, which says nothing about the tier. Assert on the
# settled state, with a deadline; on the last attempt, re-run it so the real
# reason is printed before failing.
assert_outputs() {
  api /stats | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert d['rist']['received']>0, 'no RIST bytes received'
o={x['id']:x for x in d['outputs']}
for want in ('rtmp1','srt1','srt2','rtmp2','rtmp3'):
    assert want in o, 'missing output '+want
    assert o[want]['state']=='running', want+' not running: '+o[want]['state']
    assert o[want]['bytes_sent']>0, want+' sent no bytes'
print('5 concurrent outputs running:', {k:o[k]['bytes_sent'] for k in ('rtmp1','srt1','srt2','rtmp2','rtmp3')})
"
}
settled=
for _ in $(seq 1 15); do
  if assert_outputs; then settled=1; break; fi
  sleep 2
done
# `assert_outputs || fail` -- NOT `assert_outputs; fail`. Under `set -e` a failing
# command exits the script on the spot, so the second form never reaches `fail`
# and the whole diagnostic dump is unreachable (which is why a real bus error went
# missing twice). The || is what routes the failure INTO the handler.
if [ -z "$settled" ]; then assert_outputs || fail "concurrent output assertions"; fi

# ---- Claim 4: the transcode outputs run on the GPU, not on the CPU -----------
# The first production use is a node on hardware, so "it transcoded" is not the
# bar: a silent fallback to x264enc/x265enc would pass a codec check and still be
# the wrong deployment. /status names the encoder actually chosen (MT1.8).
api /stats | python3 -c "
import json,sys
o={x['id']:x for x in json.load(sys.stdin)['outputs']}
got={k:(o[k].get('transcode') or {}).get('encoder') for k in ('rtmp1','srt1','rtmp2','rtmp3')}
print('transcode encoders chosen:', got)
assert got['rtmp1']=='vah264enc', 'rtmp1 did NOT use hardware H.264 encode -- got '+str(got['rtmp1'])+' (CPU fallback)'
assert got['srt1']=='vah265enc', 'srt1 did NOT use hardware H.265 encode -- got '+str(got['srt1'])+' (CPU fallback)'
assert got['rtmp2']=='vah264enc', 'the SCALED output did not use hardware H.264 -- got '+str(got['rtmp2'])
assert got['rtmp3']=='vah265enc', 'the h265-over-RTMP output did not use hardware H.265 -- got '+str(got['rtmp3'])
assert (o['srt2'].get('transcode') or {}).get('encoder') in (None,''), 'srt2 is a COPY output but reports a transcoder'
print('hardware encode confirmed on all four transcode outputs')
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
# ffprobe names H.265 "hevc"; GStreamer calls it h265. Accept either -- the
# check is about the codec on the wire, not about the spelling.
assert hc.get('video') in ('h265','hevc'), 'srt1 video not h265/hevc (transcode): '+str(hc)
# AV1-in-MPEG-TS uses the custom stream_type mapping that mpegtsmux requires, and
# ffprobe has no descriptor for it: it classifies the stream as codec_type DATA with
# codec_name "bin_data", not as a video stream at all -- so ac['video'] is ABSENT.
# Accept the copy as either a recognised av1 stream or that unnamed data stream, and
# treat a named h264/hevc as the real failure (it was re-encoded).
assert 'av1' in ac.values() or 'bin_data' in ac.values(), \
    'srt2 video is not the AV1 COPY -- it was re-encoded or dropped: '+str(ac)
print('srt1 ->', hc, ' srt2 ->', ac)
" || fail "transcode/copy targets wrong"

# ---- Claim 6: the scaled output is actually scaled --------------------------
# /stats names the encoder but says nothing about the scaler, so asserting the
# pipeline chose vapostproc would only prove a template was filled in. The size is
# checked on the bytes that reached the sink: 1080p in, 1440p out.
echo "== probe the scaled output at SRS (expect 2560x1440) =="
$COMPOSE run --rm -T prober \
  ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of json \
  "rtmp://${srs_ip}:1935/live/scale" > /tmp/mt19-scale.json \
  || fail "the scaled output is not playable at SRS"
python3 -c "
import json
d=json.load(open('/tmp/mt19-scale.json'))
s=(d.get('streams') or [{}])[0]
print('rtmp2 ->', s.get('codec_name'), s.get('width'), 'x', s.get('height'))
assert s.get('width')==2560 and s.get('height')==1440, \
    'the scaled output is NOT 1440p -- the upscale did not happen: '+str(s)
assert s.get('codec_name')=='h264', 'scaled output is not h264: '+str(s)
" || fail "upscale not applied to the output bytes"

# ---- Claim 7: H.265 over Enhanced RTMP is ACCEPTED and running ---------------
# flvmux is h264-only, so an h265 target on rtmp is only possible via eflvmux, and
# the receiver refuses it outright unless --allow-enhanced-rtmp is set. This claim
# exercises that gate: with the flag the target is accepted and the output carries
# hardware h265, without it /start returns bad_enum (covered by the previous rig
# run's failure mode).
#
# It deliberately does NOT assert the payload decodes. The rig's prober is
# ubuntu:24.04 (ffmpeg 6.1), whose FLV demuxer does not implement Enhanced RTMP --
# it returns EMPTY streams for the enhanced codec id rather than erroring, so a
# codec assertion here would fail on the tool, not on the product. Verifying the
# H.265 payload end to end needs an ffmpeg that implements Enhanced RTMP (7.x), or
# the real destination. Claiming more than that would be claiming the probe.
echo "== H.265-over-RTMP output is accepted, running, and on hardware h265 =="
api /stats | python3 -c "
import json,sys
o={x['id']:x for x in json.load(sys.stdin)['outputs']}
r3=o.get('rtmp3')
assert r3 is not None, 'rtmp3 is absent from /stats -- the h265-on-rtmp target was not started'
assert r3['state']=='running', 'rtmp3 not running: '+str(r3['state'])
assert r3['bytes_sent']>0, 'rtmp3 sent no bytes -- the enhanced-RTMP output is not carrying'
assert (r3.get('transcode') or {}).get('encoder')=='vah265enc', \
    'rtmp3 did not use hardware h265 -- got '+str((r3.get('transcode') or {}).get('encoder'))
print('rtmp3 accepted + running on', r3['transcode']['encoder'], 'bytes', r3['bytes_sent'])
" || fail "H.265 over Enhanced RTMP was not accepted or is not carrying"
echo "  NOTE: payload decode is not asserted -- the prober's ffmpeg 6.1 cannot read"
echo "        Enhanced RTMP H.265 (returns empty streams). Tool limit, not product."

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
