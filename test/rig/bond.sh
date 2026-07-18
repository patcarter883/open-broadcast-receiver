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
#      NETEM_A (default "delay 40ms 10ms loss 2%"),
#      NETEM_B (default "delay 120ms 30ms loss 5%").
set -euo pipefail
: "${RECV_A:?}" "${RECV_B:?}"
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
shape "$RECV_A" "$NETEM_A"
shape "$RECV_B" "$NETEM_B"

# Two outputs, SAME port on both paths — bonded links present as multiple
# RIST peers on the single session port; port fans are not supported
# (CONTRACT.md §4).
exec rist2rist \
  -i "rist://@0.0.0.0:${LISTEN_PORT}?timing-mode=0" \
  -o "rist://${RECV_A}:${RECV_PORT}?timing-mode=0" \
  -o "rist://${RECV_B}:${RECV_PORT}?timing-mode=0"
