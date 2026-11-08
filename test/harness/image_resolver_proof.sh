#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# image_resolver_proof.sh — prove the runtime image ranks IPv4 first for a name
# that answers both families, with no `--add-host` pin anywhere.
#
# The image ships /etc/gai.conf with `precedence ::ffff:0:0/96 100` because the
# node network carries no IPv6 route and glibc's RFC 6724 sort returns AAAA
# first whenever the container holds a routable-looking IPv6 address
# (DECISIONS §20). A single-stack container cannot show that: glibc's rule 3
# has already demoted the family there, and the file looks inert. This script
# therefore runs the image on an IPv6 network, which is the shape that breaks.
#
# It runs the SAME image twice and judges the difference:
#
#   with the layer    -> the answer must start with IPv4
#   file emptied      -> the answer must start with IPv6
#
# The second run is what makes the first meaningful: if both orders are IPv4,
# the harness is measuring the network rather than the file.
#
# Nothing here contacts a platform. It resolves a name and optionally opens a
# TCP connection to the first address the resolver returns.
#
# Usage:
#   test/harness/image_resolver_proof.sh [--image IMAGE] [--host NAME]
#                                        [--connect PORT]
#
# Env overrides: IMAGE (default obr-receiver:proof), PROBE_HOST
#                (default a.rtmp.youtube.com), PROBE_PORT (default 1935),
#                PROBE_NET (default obr-v6probe).
#
# --connect PORT opens a TCP connection by name to the resolver's first
# answer (default: the same port as PROBE_PORT); --connect 0 skips it. The
# check is a real connect(), so it shows the resolver's order is also the
# family the socket uses.
#
# Exit status: 0 = IPv4 first with the layer, IPv6 first without it;
#              1 = an assertion failed; 2 = the proof could not run.

set -uo pipefail

IMAGE="${IMAGE:-obr-receiver:proof}"
PROBE_HOST="${PROBE_HOST:-a.rtmp.youtube.com}"
PROBE_PORT="${PROBE_PORT:-1935}"
PROBE_NET="${PROBE_NET:-obr-v6probe}"
CONNECT_PORT="${CONNECT_PORT:-$PROBE_PORT}"

die() { printf 'resolver-proof: %s\n' "$*" >&2; exit 2; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)   IMAGE="$2"; shift 2 ;;
    --host)    PROBE_HOST="$2"; shift 2 ;;
    --connect) CONNECT_PORT="$2"; shift 2 ;;
    -h|--help) sed -n '5,36p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

command -v docker >/dev/null 2>&1 || die "docker is not available"
docker image inspect "$IMAGE" >/dev/null 2>&1 \
  || die "image ${IMAGE} is not in the local store (build it first: make image TAG=proof)"

# The file is the thing under test: an image without it would make the
# "with" run pass on the network's shape alone.
docker run --rm --entrypoint sh "$IMAGE" -c 'test -s /etc/gai.conf' \
  || die "${IMAGE} has no non-empty /etc/gai.conf; nothing to prove"

created_net=0
cleanup() {
  if [[ "$created_net" -eq 1 ]]; then
    docker network rm "$PROBE_NET" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

if ! docker network inspect "$PROBE_NET" >/dev/null 2>&1; then
  docker network create --ipv6 --subnet 2001:db8:77::/64 "$PROBE_NET" >/dev/null \
    || die "could not create IPv6 network ${PROBE_NET}"
  created_net=1
fi

# One getaddrinfo answer, printed verbatim and then reduced to its family
# sequence so an order is readable as a claim rather than as a block of text.
# `getent ahosts` emits one line per address in the order getaddrinfo returned
# them; RTMP's GSocketClient path consumes exactly that order.
probe() {
  local label=$1 prelude=$2
  local out
  out=$(docker run --rm --network "$PROBE_NET" --user 0:0 \
          --entrypoint bash "$IMAGE" -c "${prelude} getent ahosts ${PROBE_HOST}") \
    || die "getent failed in the container (${label})"
  [[ -n "$out" ]] || fail "${label}: getent returned nothing for ${PROBE_HOST}"
  printf '%s\n' "$out"
  printf 'order  %s\n' "$(printf '%s\n' "$out" \
    | awk '{print ($1 ~ /:/ ? "v6" : "v4")}' | uniq | tr '\n' ' ')"
  printf 'first  %s\n' "$(printf '%s\n' "$out" | awk 'NR==1{print $1}')"
}

echo "=== image  ${IMAGE} ==="
echo "=== host   ${PROBE_HOST} (dual-stack name, no --add-host) ==="
echo "=== net    ${PROBE_NET} (IPv6 GUA on the interface, no IPv6 route out) ==="
echo

echo "--- with the image's /etc/gai.conf ---"
docker run --rm --entrypoint sh "$IMAGE" -c 'cat /etc/gai.conf'
with_out=$(probe "with layer" "")
printf '%s\n' "$with_out"

with_order=$(printf '%s\n' "$with_out" | grep '^order' | awk '{print $2}')
[[ "$with_order" == "v4" ]] \
  || fail "with the layer the answer starts with ${with_order:-nothing}, not v4"

echo
echo "--- same image, /etc/gai.conf emptied ---"
without_out=$(probe "without layer" ': > /etc/gai.conf;')
printf '%s\n' "$without_out"

without_order=$(printf '%s\n' "$without_out" | grep '^order' | awk '{print $2}')
[[ "$without_order" == "v6" ]] \
  || fail "with the file emptied the answer starts with ${without_order:-nothing}, not v6 (the name has no AAAA answer to reorder, so the run proves nothing)"

echo
echo "--- connect to the resolver's first answer, port ${CONNECT_PORT:-skipped} ---"
# The socket is opened to the FIRST address getaddrinfo returned, as a literal,
# so the family of that answer is the family the socket uses. That is the shape
# rtmp2sink has: one address, no happy-eyeballs fallback.
connect_probe() {
  local prelude=$1
  docker run --rm --network "$PROBE_NET" --user 0:0 --entrypoint bash "$IMAGE" -c "
    set -u
    ${prelude}
    first=\$(getent ahosts ${PROBE_HOST} | awk 'NR==1{print \$1}')
    echo \"first answer \${first}\"
    if timeout 5 bash -c \"exec 3<>/dev/tcp/\${first}/${CONNECT_PORT}\" 2>/dev/null; then
      echo \"connect \${first}:${CONNECT_PORT} OK\"
      exit 0
    fi
    echo \"connect \${first}:${CONNECT_PORT} FAILED\"
    exit 1
  "
}

if [[ -z "$CONNECT_PORT" ]]; then
  echo "skipped (--connect 0)"
else
  if with_conn=$(connect_probe ""); then
    printf '%s\n' "$with_conn"
  else
    printf '%s\n' "$with_conn"
    fail "the layer's first answer for ${PROBE_HOST}:${CONNECT_PORT} did not accept a connection"
  fi

  # Informational, not asserted: whether a v6-first answer is reachable out of
  # this host is a property of the host (a node has no IPv6 route; a dev box
  # often does), and the ordering above is what the image controls.
  without_conn=$(connect_probe ': > /etc/gai.conf;')
  printf '%s\n' "$without_conn" | sed 's/^/  (no layer) /'
fi

echo
echo "PASS: IPv4 first with the layer (${with_order}), IPv6 first without it (${without_order})."
echo "      No --add-host was passed to either run."
