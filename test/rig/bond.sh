#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Rig bonding hop: one RIST input fanned to TWO peers into the SAME receiver
# port, one per WAN path (mirrors luci-app-rist2rist's miface-per-WAN model),
# with tc netem shaping each path. Requires NET_ADMIN.
#
# Env: LISTEN_PORT (default 6000), RECV_A, RECV_B (receiver IPs on wan_a/wan_b),
#      RECV_PORT (default 5000),
#      LEGS (1 = single-link bridge: no bonding, one egress, only RECV_A used,
#            and NO shaping applied here — the harness owns the impairment for
#            that shape so it can start clean and inject mid-test),
#      NETEM_A (default "delay 40ms 10ms loss 2%"),
#      NETEM_B (default "delay 120ms 30ms loss 5%").
set -euo pipefail
: "${RECV_A:?}"
LEGS="${LEGS:-2}"
if [ "$LEGS" != "1" ]; then : "${RECV_B:?}"; fi
LISTEN_PORT="${LISTEN_PORT:-6000}"
RECV_PORT="${RECV_PORT:-5000}"
NETEM_A="${NETEM_A:-delay 40ms 10ms loss 2%}"
NETEM_B="${NETEM_B:-delay 120ms 30ms loss 5%}"

# Identify which interface routes to each receiver IP, then shape it.
shape() {
  local dst="$1" spec="$2"
  local dev
  dev=$(ip -o route get "$dst" | sed -n 's/.* dev \([^ ]*\).*/\1/p')
  tc qdisc replace dev "$dev" root netem $spec
  echo "bond: netem [$spec] on $dev (-> $dst)"
}
if [ "$LEGS" = "1" ]; then
  echo "bond: single leg -> $RECV_A:${RECV_PORT} (unshaped; harness injects)"
  OUTURL="rist://${RECV_A}:${RECV_PORT}?timing-mode=0"
else
  shape "$RECV_A" "$NETEM_A"
  shape "$RECV_B" "$NETEM_B"
  OUTURL="rist://${RECV_A}:${RECV_PORT}?timing-mode=0,rist://${RECV_B}:${RECV_PORT}?timing-mode=0"
fi

# Two peers on the SAME receiver port — bonded links present as multiple RIST
# peers on the single session port; port fans are not supported (CONTRACT.md
# §4). Both destinations ride ONE -o as a comma-separated list, which is the
# tool's documented fanout form.
#
# -p 2 (ADVANCED input profile) is REQUIRED, not a preference: rist2rist
# defaults its receive profile to SIMPLE, and the fork registers an OOB
# callback on the receiver context for the auth ack + wan_telemetry. Simple
# profile refuses OOB ("Out-of-band data is not support for simple profile",
# src/rist.c rist_oob_callback_set), so rist2rist exits 1 at startup and the
# rig silently starves the receiver of media. ADVANCED also matches the
# production chain, which is ADVANCED end-to-end (encoder
# RISTNetSenderSettings, receiver RISTNetReceiverSettings.mProfile).
# -P 2 is already the tool's default for the output side; stated explicitly
# so a future librist bump cannot quietly change the hop's behaviour.
#
# Do NOT pass -o twice. rist2rist's getopt rejects the repeated option: it
# exits at startup printing its usage table, so the hop forwards nothing and
# the receiver sees 0 peers. The usage string states the supported form —
# "-o | --outurl ADDRESS:PORT[,ADDR:PORT]* ... destination fanout".
exec rist2rist \
  -p 2 -P 2 \
  -i "rist://@0.0.0.0:${LISTEN_PORT}?timing-mode=0" \
  -o "$OUTURL"
