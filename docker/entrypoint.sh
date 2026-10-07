#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Map the node agent's OBR_* env (BACKPLANE §6 / node/infra/README.md) to
# receiver CLI flags. Secrets arrive as env, never argv on the host docker
# daemon (§10.4); they only become argv inside THIS container's own memory.
#
# The agent hardens the container (non-root, read-only rootfs, cap-drop,
# egress firewall); this entrypoint only translates config.
set -eu

# Hosted receivers bind 0.0.0.0 inside the container (the container IS the
# isolation boundary); the node reverse proxy fronts control TLS. A token is
# always set by the backplane, so this is not the M1.1 no-auth footgun.
set -- --bind 0.0.0.0 --rist-port 5000

[ -n "${OBR_TOKEN:-}" ]        && set -- "$@" --token "$OBR_TOKEN"
[ -n "${OBR_PSK:-}" ]          && set -- "$@" --psk "$OBR_PSK"
[ -n "${OBR_PSK_AES:-}" ]      && set -- "$@" --psk-aes "$OBR_PSK_AES"
[ -n "${OBR_RECORD_DIR:-}" ]   && set -- "$@" --record-dir "$OBR_RECORD_DIR"
[ -n "${OBR_IDLE_TIMEOUT:-}" ] && set -- "$@" --idle-timeout "$OBR_IDLE_TIMEOUT"
[ -n "${OBR_EGRESS_DENY:-}" ]  && set -- "$@" --egress-deny "$OBR_EGRESS_DENY"
# Enhanced FLV (eflvmux) is opt-in because it is the newer muxer: flvmux is the
# battle-tested path, so the receiver must not switch muxers for an operator who
# did not ask. An h265 target on rtmp/rtmps is REFUSED without this, and YouTube
# will only accept H.265 live via enhanced RTMP -- so the H.265-to-YouTube route
# requires the node to set OBR_ALLOW_ENHANCED_RTMP.
[ -n "${OBR_ALLOW_ENHANCED_RTMP:-}" ] && set -- "$@" --allow-enhanced-rtmp

exec open-broadcast-receiver "$@"
