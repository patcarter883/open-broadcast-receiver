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
#
# Two forms, and argv wins:
#   * `docker run <image> [args...]` with args -> those args become the
#     receiver's argv verbatim and NO OBR_* variable is consulted. This is the
#     escape hatch for a probe, a self-host, or any flag the mapping lacks.
#   * `docker run <image>` with no args -> the mapping below builds the argv.
# The forms are exclusive, so a flag can never arrive twice with two values.
set -eu

if [ "$#" -gt 0 ]; then
  echo "entrypoint: explicit argv given; OBR_* env is not mapped" >&2
  exec open-broadcast-receiver "$@"
fi

# ---------------------------------------------------------------------------
# Relay mode — OBR_RELAY_IN/OBR_RELAY_OUT.
#
# A relay is a packet pump, not a receiver: it takes no control port, no token
# and no bind, so this branch must return before any of the receiver flags are
# built. Both legs are required together; one alone is a misconfiguration that
# would otherwise start a process with no second leg and die on its own (or,
# worse, half-bind). Checked here so the failure is one clear line in the agent's
# log rather than a receiver argv error three layers down.
# ---------------------------------------------------------------------------
if [ -n "${OBR_RELAY_IN:-}" ] || [ -n "${OBR_RELAY_OUT:-}" ]; then
  if [ -z "${OBR_RELAY_IN:-}" ] || [ -z "${OBR_RELAY_OUT:-}" ]; then
    echo "entrypoint: relay mode needs BOTH OBR_RELAY_IN and OBR_RELAY_OUT (got OBR_RELAY_IN='${OBR_RELAY_IN:-}' OBR_RELAY_OUT='${OBR_RELAY_OUT:-}')" >&2
    exit 1
  fi
  set -- --relay-in "$OBR_RELAY_IN" --relay-out "$OBR_RELAY_OUT"
  # SRP: the verifier is generated HERE, in the container that owns the librist
  # that will validate against it. The node agent has no ristsrppasswd (it is not
  # a librist image), so asking it for one is a failure the whole relay inherits.
  # A mounted file still wins when given, for a pre-built verifier.
  srp_file="${OBR_TRUNK_SRP_FILE:-}"
  if [ -z "$srp_file" ] && [ -n "${OBR_TRUNK_SRP_USER:-}" ] && [ -n "${OBR_TRUNK_SRP_PASS:-}" ]; then
    srp_file=/tmp/relay-srp.txt
    if ! ristsrppasswd "$OBR_TRUNK_SRP_USER" "$OBR_TRUNK_SRP_PASS" > "$srp_file"; then
      echo "entrypoint: relay needs a usable SRP verifier and ristsrppasswd failed (user='${OBR_TRUNK_SRP_USER}')" >&2
      exit 1
    fi
    chmod 600 "$srp_file"
  fi
  if [ -n "$srp_file" ]; then
    set -- "$@" --relay-srp "$srp_file"
  else
    echo "entrypoint: relay mode has no SRP verifier (set OBR_TRUNK_SRP_USER + OBR_TRUNK_SRP_PASS, or OBR_TRUNK_SRP_FILE)" >&2
    exit 1
  fi
  [ -n "${OBR_TRUNK_SECRET:-}" ]   && set -- "$@" --relay-secret "$OBR_TRUNK_SECRET"
  [ -n "${OBR_TRUNK_AES:-}" ]      && set -- "$@" --relay-aes "$OBR_TRUNK_AES"
  exec open-broadcast-receiver "$@"
fi

# Hosted receivers bind 0.0.0.0 inside the container (the container IS the
# isolation boundary); the node reverse proxy fronts control TLS. A token is
# always set by the backplane, so this is not the M1.1 no-auth footgun.
#
# Trunk ingest (OBR_TRUNK_URL) flips the media direction only: the receiver
# dials the relay's listening sender and opens NO RIST listen socket, so
# --rist-port is omitted entirely rather than passed and ignored. The control
# plane is unchanged — it still binds 0.0.0.0 and still takes the token.
if [ -n "${OBR_TRUNK_URL:-}" ]; then
  set -- --bind 0.0.0.0 --connect-url "$OBR_TRUNK_URL"
  # SRP client pair for the relay's authenticated sender leg. Both or neither:
  # the receiver refuses half a pair, so passing one through would only move the
  # error later. Only meaningful in trunk mode (the listener authenticates its
  # callers instead), which is why they are inside this branch.
  if [ -n "${OBR_TRUNK_SRP_USER:-}" ] && [ -n "${OBR_TRUNK_SRP_PASS:-}" ]; then
    set -- "$@" --srp-user "$OBR_TRUNK_SRP_USER" --srp-pass "$OBR_TRUNK_SRP_PASS"
  elif [ -n "${OBR_TRUNK_SRP_USER:-}" ] || [ -n "${OBR_TRUNK_SRP_PASS:-}" ]; then
    echo "entrypoint: OBR_TRUNK_SRP_USER and OBR_TRUNK_SRP_PASS must be set together" >&2
    exit 1
  fi
else
  set -- --bind 0.0.0.0 --rist-port 5000
fi

[ -n "${OBR_TOKEN:-}" ]        && set -- "$@" --token "$OBR_TOKEN"
[ -n "${OBR_PSK:-}" ]          && set -- "$@" --psk "$OBR_PSK"
[ -n "${OBR_PSK_AES:-}" ]      && set -- "$@" --psk-aes "$OBR_PSK_AES"
[ -n "${OBR_RECORD_DIR:-}" ]   && set -- "$@" --record-dir "$OBR_RECORD_DIR"
[ -n "${OBR_IDLE_TIMEOUT:-}" ] && set -- "$@" --idle-timeout "$OBR_IDLE_TIMEOUT"
[ -n "${OBR_EGRESS_DENY:-}" ]  && set -- "$@" --egress-deny "$OBR_EGRESS_DENY"
# Private/LAN destinations (an on-prem restreamer, a LAN RTMP sink, a local
# mediamtx) are refused by the SSRF policy unless the operator opts in. Deny
# stays the default: only an explicit 1/true/yes/on adds --egress-allow-private.
# Loopback, link-local and multicast are refused by the receiver even then, so
# this flag cannot be used to reach the container's own services.
case "$(printf '%s' "${OBR_EGRESS_ALLOW_PRIVATE:-}" | tr '[:upper:]' '[:lower:]')" in
  "" | 0 | false | no | off) ;;
  1 | true | yes | on)       set -- "$@" --egress-allow-private ;;
  *)
    echo "entrypoint: OBR_EGRESS_ALLOW_PRIVATE='${OBR_EGRESS_ALLOW_PRIVATE}' is not 1/true/yes/on; private destinations stay denied" >&2
    ;;
esac
# Enhanced FLV (eflvmux) is opt-in because it is the newer muxer: flvmux is the
# battle-tested path, so the receiver must not switch muxers for an operator who
# did not ask. An h265 target on rtmp/rtmps is REFUSED without this, and YouTube
# will only accept H.265 live via enhanced RTMP -- so the H.265-to-YouTube route
# requires the node to set OBR_ALLOW_ENHANCED_RTMP.
[ -n "${OBR_ALLOW_ENHANCED_RTMP:-}" ] && set -- "$@" --allow-enhanced-rtmp
# Standby slate. The node sets OBR_STANDBY=1 when the plan wants a slate on
# air; anything but an explicit 0/false forces it on (absent included, so a
# node that predates the variable keeps the slate). OBR_STANDBY=0 is the
# operator kill switch: --no-standby beats both the /start body and the
# default. The brand image is a path inside this container.
case "$(printf '%s' "${OBR_STANDBY:-}" | tr '[:upper:]' '[:lower:]')" in
  0 | false) set -- "$@" --no-standby ;;
  *)         set -- "$@" --standby-slate ;;
esac
[ -n "${OBR_STANDBY_BRAND:-}" ] && set -- "$@" --standby-brand "$OBR_STANDBY_BRAND"

exec open-broadcast-receiver "$@"
