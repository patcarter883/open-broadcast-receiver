#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# relay_trunk_e2e.sh <receiver-binary> <connect-ingest-probe>
#
# The trunk chain, end to end, with real RIST on the wire:
#
#   python feeder --udp--> ristsender --rist--> relay --rist--> probe (connect mode)
#        (venue)             (venue sender)      (this repo)      (this repo)
#
# What it proves, and why it is judged this way:
#
#   * Both relay legs BIND ('@'), and the far end DIALS. That is the only shape
#     that works for a BYON venue behind NAT: nothing can dial the node, so the
#     relay's sender leg must be the listener.
#   * Sequence continuity is preserved ACROSS the relay. Packets sent before the
#     three-hop handshake completes are legitimately dropped (librist holds data
#     until the peer authenticates), so the assertion is: every sequence number
#     at or above the first one received arrives, with no gaps. "It connected"
#     is not a result — a relay that restamps or reorders passes "connected" and
#     fails this.
#   * The receiver's connect mode authenticates with the relay's SRP file using
#     librist's URL parameters (`username=` / `password=`). SRP is ON in this
#     harness; a receiver that sends no credentials receives nothing.
#
# Exit 0 = pass. Exit 77 = a required tool is missing (skipped).
set -uo pipefail

RECEIVER="${1:?usage: relay_trunk_e2e.sh <receiver-binary> <probe-binary>}"
PROBE="${2:?usage: relay_trunk_e2e.sh <receiver-binary> <probe-binary>}"

for tool in ristsender ristsrppasswd python3; do
  command -v "${tool}" >/dev/null 2>&1 || {
    echo "relay_trunk_e2e: SKIP (missing ${tool})" >&2
    exit 77
  }
done
[ -x "${RECEIVER}" ] || { echo "relay_trunk_e2e: no receiver binary: ${RECEIVER}" >&2; exit 1; }
[ -x "${PROBE}" ] || { echo "relay_trunk_e2e: no probe binary: ${PROBE}" >&2; exit 1; }

work="$(mktemp -d)"
pids=()
cleanup() {
  for pid in "${pids[@]:-}"; do
    [ -n "${pid}" ] && kill "${pid}" 2>/dev/null
  done
  wait 2>/dev/null
  if [ "${KEEP:-0}" = "1" ]; then
    echo "relay_trunk_e2e: artefacts kept in ${work}" >&2
  else
    rm -rf "${work}"
  fi
}
trap cleanup EXIT

fail=0
note() { echo "relay_trunk_e2e: $*"; }
die()  { echo "relay_trunk_e2e: FAIL: $*" >&2; exit 1; }

# Free UDP ports, so this can run beside other agents' harnesses.
read -r IN_PORT OUT_PORT FEED_PORT < <(python3 - <<'PY'
import socket
ports = []
socks = []
for _ in range(3):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    socks.append(s)
    ports.append(s.getsockname()[1])
print(*ports)
PY
)

# SRP credential file for the relay's out leg (authenticating the probe).
ristsrppasswd trunkuser trunkpass > "${work}/srp.txt" || die "ristsrppasswd failed"
[ -s "${work}/srp.txt" ] || die "SRP file is empty"
note "SRP file: $(wc -l < "${work}/srp.txt") line(s)"

# ---- relay ---------------------------------------------------------------
"${RECEIVER}" \
  --relay-in "rist://@127.0.0.1:${IN_PORT}?timing-mode=0" \
  --relay-out "rist://@127.0.0.1:${OUT_PORT}?timing-mode=0" \
  --relay-srp "${work}/srp.txt" \
  > "${work}/relay.log" 2>&1 &
relay_pid=$!
pids+=("${relay_pid}")

for _ in $(seq 1 100); do
  grep -q "both legs bound" "${work}/relay.log" 2>/dev/null && break
  kill -0 "${relay_pid}" 2>/dev/null || die "relay died: $(tail -5 "${work}/relay.log")"
  sleep 0.1
done
grep -q "both legs bound" "${work}/relay.log" \
  || die "relay never reported both legs bound: $(tail -5 "${work}/relay.log")"
# Both legs must have LOGGED the listening direction, and the sender must have
# reported "Initialized Sender Peer, listening mode" before any media — that is
# the bind proof the brief asks for.
grep -q "in leg URL listening mode" "${work}/relay.log" \
  || die "relay in leg did not report listening mode"
grep -q "out leg URL listening mode" "${work}/relay.log" \
  || die "relay out leg did not report listening mode"
grep -q "Initialized Sender Peer, listening mode" "${work}/relay.log" \
  || die "relay out leg is not a bound sender peer"
note "relay bound: in=@:${IN_PORT} out=@:${OUT_PORT} (both listening)"

# ---- venue sender --------------------------------------------------------
# The relay authenticates BOTH legs (a venue sender is the untrusted end), so
# the venue sender has to present the same SRP pair the receiver does. The
# rist tools have no client-side SRP flags — the only channel is the URL
# parameters librist's parser reads (RIST_URL_PARAM_SRP_USERNAME/_PASSWORD),
# which is exactly the channel the receiver's connect mode uses.
ristsender \
  -i "udp://127.0.0.1:${FEED_PORT}" \
  -o "rist://127.0.0.1:${IN_PORT}?timing-mode=0&username=trunkuser&password=trunkpass" \
  -p 2 -S 0 -f 1 \
  > "${work}/sender.log" 2>&1 &
pids+=("$!")
sleep 1

# ---- receiver in CONNECT mode -------------------------------------------
"${PROBE}" \
  --connect-url "rist://127.0.0.1:${OUT_PORT}" \
  --srp-user trunkuser --srp-pass trunkpass \
  --out "${work}/capture.bin" --seconds 22 \
  > "${work}/probe.out" 2> "${work}/probe.log" &
probe_pid=$!
pids+=("${probe_pid}")

for _ in $(seq 1 50); do
  grep -q "RIST connecting to" "${work}/probe.log" 2>/dev/null && break
  sleep 0.1
done
grep -q "RIST connecting to: rist://127.0.0.1:${OUT_PORT}" "${work}/probe.log" \
  || die "receiver did not enter connect mode: $(head -5 "${work}/probe.log")"
# Connect mode must NOT open a listen socket: if it did, the far end would be
# dialling itself and the media would never traverse the relay.
if grep -q "RIST listening on" "${work}/probe.log"; then
  die "connect-mode receiver also reported listening — it bound a socket"
fi
note "receiver dialling: rist://127.0.0.1:${OUT_PORT} (connect mode)"

# ---- feed 600 packets at 100/s, starting after the handshake ------------
# 6 s of media, then a long quiet tail before the probe stops: the last packets
# have to clear the relay's queue AND the receiver's reorder/recovery buffer, or
# they look like loss when they are merely in flight. A tight tail here is the
# one way this harness flakes on a loaded box (it did, beside stage_test), so
# the margin is deliberately ~14 s rather than "just enough".
sleep 2
python3 - "${FEED_PORT}" 600 <<'PY' &
import socket, struct, sys, time
port, count = int(sys.argv[1]), int(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for seq in range(count):
    # 188-byte MPEG-TS-shaped packet: sync byte, then a 32-bit big-endian
    # sequence we control, so loss can be attributed to an exact number.
    pkt = bytearray(188)
    pkt[0] = 0x47
    struct.pack_into(">I", pkt, 4, seq)
    sock.sendto(pkt, ("127.0.0.1", port))
    time.sleep(0.01)
PY
pids+=("$!")
note "feeding 600 packets"

wait "${probe_pid}" 2>/dev/null
probe_rc=$?
probe_line="$(cat "${work}/probe.out" 2>/dev/null)"
note "probe: ${probe_line}"
[ "${probe_rc}" -eq 0 ] || die "probe exited ${probe_rc}: $(tail -5 "${work}/probe.log")"

# ---- judge: count in vs out, and where any loss sits --------------------
python3 - "${work}/capture.bin" "${work}/relay.log" <<'PY'
import re, sys
cap_path, relay_log = sys.argv[1], sys.argv[2]
data = open(cap_path, "rb").read()
if len(data) % 188 != 0:
    print(f"relay_trunk_e2e: FAIL: captured {len(data)} bytes, not a whole "
          f"number of 188-byte packets", file=sys.stderr)
    sys.exit(1)
pkts = [data[i:i + 188] for i in range(0, len(data), 188)]
seqs = [int.from_bytes(p[4:8], "big") for p in pkts]
sent = 600
if not seqs:
    print("relay_trunk_e2e: FAIL: nothing received through the trunk",
          file=sys.stderr)
    sys.exit(1)
first = min(seqs)
received = set(seqs)
missing = [s for s in range(sent) if s not in received]
# Loss is only allowed BEFORE the first delivered packet: those were in flight
# while the three-hop handshake completed, which no relay can avoid.
post_handshake_loss = [s for s in missing if s >= first]
dupes = len(seqs) - len(received)
# Relay-side counters, straight from its own log.
counters = dict(re.findall(r"(in_packets|in_bytes|out_packets|out_bytes)=(\d+)",
                           open(relay_log, errors="replace").read()[-4000:]))
print(f"relay_trunk_e2e: sent={sent} received={len(received)} "
      f"first_seq={first} missing={len(missing)} "
      f"missing_range={'0-'+str(first-1) if first else 'none'} "
      f"dupes={dupes} relay_in_packets={counters.get('in_packets', '?')} "
      f"relay_out_packets={counters.get('out_packets', '?')}")
if post_handshake_loss:
    print(f"relay_trunk_e2e: FAIL: loss after the handshake at seq "
          f"{post_handshake_loss[:20]}", file=sys.stderr)
    sys.exit(1)
if dupes:
    print(f"relay_trunk_e2e: FAIL: {dupes} duplicate sequence number(s)",
          file=sys.stderr)
    sys.exit(1)
if received != set(range(first, sent)):
    print("relay_trunk_e2e: FAIL: sequence set is not contiguous after the "
          f"handshake (got {len(received)}, expected {sent - first})",
          file=sys.stderr)
    sys.exit(1)
PY
rc=$?
[ "${rc}" -eq 0 ] || fail=1

# ---- negative control: the same chain with a WRONG password -------------
# A harness that passes without authentication is not testing authentication.
# The relay still has media flowing from the venue, so the only variable is the
# probe's credential.
note "negative control: wrong SRP password"
"${PROBE}" \
  --connect-url "rist://127.0.0.1:${OUT_PORT}" \
  --srp-user trunkuser --srp-pass definitely-not-the-password \
  --out "${work}/capture-bad.bin" --seconds 5 \
  > "${work}/probe-bad.out" 2> "${work}/probe-bad.log"
bad_rc=$?
bad_line="$(cat "${work}/probe-bad.out" 2>/dev/null)"
bad_bytes="$(wc -c < "${work}/capture-bad.bin" 2>/dev/null || echo 0)"
note "negative control: exit=${bad_rc} ${bad_line}"
if [ "${bad_rc}" -eq 0 ] || [ "${bad_bytes}" -ne 0 ]; then
  echo "relay_trunk_e2e: FAIL: a wrong SRP password still carried ${bad_bytes} bytes" >&2
  fail=1
fi
grep -q "Authentication failed" "${work}/probe-bad.log" \
  || { echo "relay_trunk_e2e: FAIL: wrong password produced no auth failure" >&2; fail=1; }

if [ "${fail}" -ne 0 ]; then
  echo "relay_trunk_e2e: FAILED" >&2
  exit 1
fi
note "PASS: bytes and sequence continuity hold across the relay; wrong credentials carry nothing"
exit 0
