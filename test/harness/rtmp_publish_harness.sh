#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Start a local RTMP sink, run the publish harness against it, tear the sink
# down again. Nothing here contacts the capture box, the RIST ingress or a
# platform: the only moving parts are this container and the harness binary.
#
# Usage:
#   test/harness/rtmp_publish_harness.sh [--bin build/test/rtmp_publish_harness]
#                                        [--modes "transcode-av copy-av"]
#
# Env overrides: HARNESS_BIN, MODES, RTMP_PORT, STAT_PORT, IMAGE, LOG_DIR,
# FIXTURE (AV1-in-MPEG-TS clip required by the transcode-av1 mode; generate it
# with `make fixture`).
# Exit status: 0 when every mode published (judged by the sink's own bytes_in),
# 1 otherwise,
# 2 when the harness could not be run at all (no docker, no sink, no binary).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS_BIN="${HARNESS_BIN:-${REPO_ROOT}/build/test/rtmp_publish_harness}"
MODES="${MODES:-transcode-av transcode-video-only copy-av copy-audio-first copy-non-aac copy-video-only}"
RTMP_PORT="${RTMP_PORT:-11935}"
STAT_PORT="${STAT_PORT:-18080}"
IMAGE="${IMAGE:-alfg/nginx-rtmp:latest}"
LOG_DIR="${LOG_DIR:-${REPO_ROOT}/build/rtmp-harness-logs}"
FIXTURE="${FIXTURE:-}"
CONTAINER="obr-rtmp-sink-$$"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin) HARNESS_BIN="$2"; shift 2 ;;
    --modes) MODES="$2"; shift 2 ;;
    -h|--help) sed -n '5,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

command -v docker >/dev/null 2>&1 || { echo "docker is not available" >&2; exit 2; }
[[ -x "$HARNESS_BIN" ]] || { echo "harness binary not built: $HARNESS_BIN" >&2; exit 2; }

# A verdict is only meaningful for the code the binary was linked against. A
# binary older than its sources measures a commit nobody is running, and its
# numbers cannot be told apart from a live regression. Refuse to run it.
# Only compiled sources are compared: a CMakeLists edit that changes nothing the
# harness links would otherwise block every run, because a build cannot refresh
# a binary it does not relink.
newer_source="$(find "$REPO_ROOT/source" "$REPO_ROOT/test/source" \
                     -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
                     -newer "$HARNESS_BIN" -print -quit 2>/dev/null)"
if [[ -n "$newer_source" ]]; then
  echo "harness binary is older than the sources it must exercise:" >&2
  echo "  binary  ${HARNESS_BIN} ($(date -r "$HARNESS_BIN" '+%F %T'))" >&2
  echo "  newer   ${newer_source} ($(date -r "$newer_source" '+%F %T'))" >&2
  echo "rebuild it first: cmake --build $(dirname "$(dirname "$HARNESS_BIN")")" >&2
  exit 2
fi

mkdir -p "$LOG_DIR"

cleanup() {
  if docker rm -f "$CONTAINER" >/dev/null 2>&1; then
    echo "removed container ${CONTAINER} (${IMAGE})"
  fi
}
trap cleanup EXIT INT TERM

echo "=== starting RTMP sink ==="
echo "image      ${IMAGE}"
# The binary's own stamp names the commit it was linked against; this names the
# tree being measured, so a mismatch is visible in the report.
source_commit="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
source_branch="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo -)"
echo "source     ${source_commit} on ${source_branch}"
echo "container  ${CONTAINER}"
echo "command    docker run -d --rm --name ${CONTAINER} \\"
echo "             -p ${RTMP_PORT}:1935 -p ${STAT_PORT}:8080 \\"
echo "             -v docker/rtmp-sink/nginx.conf:/etc/nginx/harness.conf:ro \\"
echo "             ${IMAGE} nginx -c /etc/nginx/harness.conf -g 'daemon off;'"

# The image's entrypoint rewrites /etc/nginx/nginx.conf, so the harness config is
# mounted elsewhere and passed with -c instead.
if ! docker run -d --rm --name "$CONTAINER" \
    -p "${RTMP_PORT}:1935" -p "${STAT_PORT}:8080" \
    -v "${REPO_ROOT}/docker/rtmp-sink/nginx.conf:/etc/nginx/harness.conf:ro" \
    "$IMAGE" nginx -c /etc/nginx/harness.conf -g 'daemon off;' >/dev/null; then
  echo "could not start the RTMP sink container" >&2
  exit 2
fi

ready=0
for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:${STAT_PORT}/healthz" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.2
done
if [[ "$ready" -ne 1 ]]; then
  echo "RTMP sink did not answer on 127.0.0.1:${STAT_PORT}" >&2
  docker logs "$CONTAINER" 2>&1 | tail -20
  exit 2
fi
echo "sink ready: rtmp://127.0.0.1:${RTMP_PORT}/live/test, stats http://127.0.0.1:${STAT_PORT}/stat"

failures=0
results=()
for mode in $MODES; do
  log="${LOG_DIR}/${mode}.log"
  echo
  echo "=== mode ${mode} ==="
  mode_args=()
  if [[ "$mode" == "transcode-av1" ]]; then
    if [[ -z "$FIXTURE" ]]; then
      echo "mode transcode-av1 needs FIXTURE=<av1.ts> (make fixture)" >&2
      exit 2
    fi
    mode_args=(--fixture "$FIXTURE")
    # FIXTURE_AUDIO names what the file carries, the same way the fixture
    # generator does: a clip with AAC must satisfy the audio assertions.
    if [[ "${FIXTURE_AUDIO:-1}" != "0" ]]; then
      mode_args+=(--fixture-audio)
    fi
  fi
  if "$HARNESS_BIN" --mode "$mode" \
      --host 127.0.0.1 --port "$RTMP_PORT" \
      --stat-url "http://127.0.0.1:${STAT_PORT}/stat" \
      "${mode_args[@]}" 2>&1 | tee "$log"; then
    verdict="PASS"
  else
    verdict="FAIL"
    failures=$((failures + 1))
  fi
  results+=("$(printf '%-22s %s' "$mode" "$verdict")")
done

echo
echo "=== summary ==="
for line in "${results[@]}"; do
  echo "$line"
done
echo "logs       ${LOG_DIR}"
if [[ "$failures" -ne 0 ]]; then
  echo "result     ${failures} mode(s) did not publish"
  exit 1
fi
echo "result     all modes published"
