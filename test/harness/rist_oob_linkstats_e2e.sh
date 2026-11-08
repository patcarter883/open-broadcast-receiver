#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# rist_oob_linkstats_e2e.sh — prove, on one box and with no network, that the
# link statistics the receiver measures reach the encoder's read path.
#
#   rist_feed (encoder stand-in) -> rist2rist (bridge) -> receiver
#
# Three things are measured, all from real processes:
#   * the receiver's GET /stats `rist` block carries live quality/rtt/received/
#     bandwidth and per-peer entries while the feed runs, and `received` grows
#     between two samples;
#   * the bridge logs the 5-byte wan_telemetry it receives from the receiver on
#     its OOB channel ("receiver link telemetry: ..."), so the last-leg
#     observation is not dropped at the relay;
#   * rist_feed, which decodes OOB exactly as the encoder does (5-byte packed
#     struct, big-endian RTT), prints `oob link_quality=<q> rtt_ms=<rtt>`.
#
# Usage: test/harness/rist_oob_linkstats_e2e.sh [seconds]
#   Env: RECEIVER  (default build/open-broadcast-receiver)
#        FEED      (default build/test/rist_feed)
#        RIST2RIST (default: `command -v rist2rist`) — the patched production
#                  bridge built from test/rig/rist2rist-tool.c; see README.md
#        CONTROL_PORT (default 18093), RIST_PORT (default 15120),
#        BRIDGE_PORT (default 15121)
# Exit: 0 pass, 1 fail, 77 a required binary is missing.

set -euo pipefail

seconds="${1:-12}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
receiver="${RECEIVER:-$root/build/open-broadcast-receiver}"
feed="${FEED:-$root/build/test/rist_feed}"
# Prefer the bridge built from this repo's (patched) source; a system rist2rist
# is the upstream tool and has no -P/--out-profile or OOB relay.
rist2rist="${RIST2RIST:-}"
if [[ -z "$rist2rist" ]]; then
  if [[ -x "$root/build/rig/rist2rist" ]]; then
    rist2rist="$root/build/rig/rist2rist"
  else
    rist2rist="$(command -v rist2rist || true)"
  fi
fi
control_port="${CONTROL_PORT:-18093}"
rist_port="${RIST_PORT:-15120}"
bridge_port="${BRIDGE_PORT:-15121}"
token="oob-e2e-token"
work="${WORK:-$root/build/oob-e2e}"

die() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 77; }

[[ -x "$receiver" ]] || die "receiver not built: $receiver  (cmake --build build)"
[[ -x "$feed" ]] || die "feed tool not built: $feed  (cmake --build build --target rist_feed)"
[[ -n "$rist2rist" && -x "$rist2rist" ]] || skip "no rist2rist; set RIST2RIST=<path> (build from test/rig/rist2rist-tool.c)"

for tool in curl jq pv ss; do
  command -v "$tool" >/dev/null 2>&1 || skip "missing $tool"
done
for port in "$control_port" "$rist_port" "$bridge_port"; do
  holder="$(ss -lunH "sport = :$port" 2>/dev/null | head -1)"
  if [[ -z "$holder" ]]; then
    holder="$(ss -ltnH "sport = :$port" 2>/dev/null | head -1)"
  fi
  [[ -z "$holder" ]] || die "port $port is already held: $holder"
done

mkdir -p "$work"
receiver_log="$work/receiver.log"
bridge_log="$work/bridge.log"
feed_log="$work/feed.log"
: >"$receiver_log"; : >"$bridge_log"; : >"$feed_log"

cleanup() {
  [[ -n "${feed_pid:-}" ]] && kill "$feed_pid" 2>/dev/null || true
  [[ -n "${bridge_pid:-}" ]] && kill "$bridge_pid" 2>/dev/null || true
  [[ -n "${receiver_pid:-}" ]] && kill "$receiver_pid" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

api() { curl -s -H "Authorization: Bearer $token" "http://127.0.0.1:$control_port$1"; }

# ---- receiver ---------------------------------------------------------------
"$receiver" --control-port "$control_port" --rist-port "$rist_port" \
  --token "$token" --egress-allow-private >>"$receiver_log" 2>&1 &
receiver_pid=$!

ready=0
for _ in $(seq 1 50); do
  if api /status >/dev/null 2>&1; then ready=1; break; fi
  sleep 0.2
done
[[ "$ready" -eq 1 ]] || { tail -20 "$receiver_log" >&2; die "control API did not come up"; }

# The output is a dead SRT port on this host's LAN address: the session must
# run and keep ingesting even while every output is retrying.
lan_ip="$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)"
[[ -n "$lan_ip" ]] || skip "no non-loopback IPv4 address for the (unused) output"

body=$(cat <<EOF
{
  "schema_version": 5,
  "session_id": "oob-e2e-$RANDOM",
  "ingest": {"bandwidth": 8000, "rist_listen": "rist://@127.0.0.1:$rist_port"},
  "source": {"codec": "av1", "width": 1280, "height": 720, "fps": 30},
  "outputs": [{"id": "dead", "type": "srt", "url": "srt://$lan_ip:19359", "key_or_streamid": "x"}]
}
EOF
)
curl -s -X POST "http://127.0.0.1:$control_port/start" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d "$body" | jq -e '.ok == true' >/dev/null || die "POST /start refused"

# ---- bridge -----------------------------------------------------------------
# -p 2 / -P 2: the LAN leg must be MAIN or ADVANCED — libRIST refuses
# rist_oob_callback_set() on a SIMPLE context, and without it the bridge cannot
# write telemetry back to the encoder at all.
"$rist2rist" -i "rist://@127.0.0.1:$bridge_port" -p 2 \
             -o "rist://127.0.0.1:$rist_port" -P 2 -v 7 >>"$bridge_log" 2>&1 &
bridge_pid=$!
sleep 1
kill -0 "$bridge_pid" 2>/dev/null || { tail -20 "$bridge_log" >&2; die "bridge exited"; }

# ---- feed -------------------------------------------------------------------
# 1.5 MB/s for `seconds`: enough for several one-second statistics windows.
pv -q -L 1500000 /dev/zero 2>/dev/null | head -c $((1500000 * seconds)) \
  | "$feed" 127.0.0.1 "$bridge_port" 2>"$feed_log" &
feed_pid=$!

sleep 3
stats_a="$(api /stats)"
sleep 3
stats_b="$(api /stats)"

jq -e '.rist' <<<"$stats_a" >/dev/null || die "/stats has no rist block"

quality="$(jq -r '.rist.quality' <<<"$stats_b")"
rtt="$(jq -r '.rist.rtt_ms' <<<"$stats_b")"
received="$(jq -r '.rist.received' <<<"$stats_b")"
bandwidth="$(jq -r '.rist.bandwidth_bps' <<<"$stats_b")"
peers="$(jq -r '.rist.peers | length' <<<"$stats_b")"
bytes_a="$(jq -r '.ts_in.bytes_total' <<<"$stats_a")"
bytes_b="$(jq -r '.ts_in.bytes_total' <<<"$stats_b")"

echo "=== /stats rist block (second sample) ==="
jq '.rist' <<<"$stats_b"

awk -v q="$quality" 'BEGIN { exit !(q >= 0 && q <= 100) }' || die "quality out of range: $quality"
[[ "$rtt" -gt 0 ]] || die "rtt_ms is not positive: $rtt"
[[ "$bandwidth" -gt 0 ]] || die "bandwidth_bps is not positive: $bandwidth"
[[ "$received" -gt 0 ]] || die "received is zero while the feed runs"
[[ "$peers" -gt 0 ]] || die "no RIST peers reported"
# `rist.received` is the last ~1 s window (receive.cpp: "the same figure the
# encoder's ABR acts on"), so it does not grow monotonically; the cumulative
# byte counter is the one that must advance.
[[ "$bytes_b" -gt "$bytes_a" ]] \
  || die "ingest bytes_total did not grow between samples ($bytes_a -> $bytes_b)"

# ---- OOB relay -----------------------------------------------------------------
grep -q "receiver link telemetry: q=" "$bridge_log" \
  || die "bridge never logged the receiver's OOB telemetry block"
grep -q "rist_feed: oob link_quality=" "$feed_log" \
  || die "encoder-side reader received no link telemetry"

oob_line="$(grep "rist_feed: oob link_quality=" "$feed_log" | tail -1)"
oob_rtt="$(sed -n 's/.*rtt_ms=\([0-9]*\).*/\1/p' <<<"$oob_line")"
[[ -n "$oob_rtt" && "$oob_rtt" -gt 0 ]] || die "relayed telemetry has no usable RTT: $oob_line"

echo
echo "=== bridge saw from the receiver ==="
grep "receiver link telemetry: q=" "$bridge_log" | tail -2
echo "=== encoder-side reader (rist_feed) ==="
grep "rist_feed: oob link_quality=" "$feed_log" | tail -2
echo
echo "PASS: quality=$quality rtt_ms=$rtt bandwidth_bps=$bandwidth peers=$peers" \
     "received=$received bytes_total=$bytes_a->$bytes_b oob_rtt_ms=$oob_rtt"
