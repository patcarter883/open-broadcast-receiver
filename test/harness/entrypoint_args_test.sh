#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# entrypoint_args_test.sh — asserts the shipped docker/entrypoint.sh honours an
# explicit argv and maps OBR_EGRESS_ALLOW_PRIVATE to --egress-allow-private
# without weakening the default deny.
#
# The receiver binary is stubbed: a fake `open-broadcast-receiver` earlier in
# PATH prints its own argv, one argument per line, so the test reads exactly what
# the entrypoint would exec. No container, no image, no GStreamer.
#
# Registered with CTest as `entrypoint_args_test`; exit 0 = pass.

set -uo pipefail

ENTRYPOINT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../docker" && pwd)/entrypoint.sh}"
[ -x "${ENTRYPOINT}" ] || { echo "entrypoint not executable: ${ENTRYPOINT}" >&2; exit 1; }

stub_dir="$(mktemp -d)"
trap 'rm -rf "${stub_dir}"' EXIT
cat > "${stub_dir}/open-broadcast-receiver" <<'STUB'
#!/bin/sh
printf '%s\n' "$@"
STUB
chmod +x "${stub_dir}/open-broadcast-receiver"

fail=0
expect_arg() {  # $1 = description, $2 = expected arg, $3 = argv dump
  if ! grep -qxF -- "$2" <<<"$3"; then
    echo "FAIL: $1: expected '$2' in argv:" >&2
    sed 's/^/    /' <<<"$3" >&2
    fail=1
  fi
}
expect_no_arg() {
  if grep -qxF -- "$2" <<<"$3"; then
    echo "FAIL: $1: '$2' must not appear in argv:" >&2
    sed 's/^/    /' <<<"$3" >&2
    fail=1
  fi
}

# run ENV=VAL... -- [receiver-argv...]: prints the stub's argv. `--` separates
# the env assignments from an explicit receiver command line.
run() {
  local -a envs=()
  while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
    envs+=("$1")
    shift
  done
  [ "${1:-}" = "--" ] && shift
  env PATH="${stub_dir}:${PATH}" "${envs[@]}" "${ENTRYPOINT}" "$@" 2>/dev/null
}

# 1. Default: hosted bind, and private destinations stay denied.
argv=$(run OBR_TOKEN=t --)
expect_arg "default bind" "--bind" "${argv}"
expect_arg "default rist port" "5000" "${argv}"
expect_no_arg "default keeps the SSRF deny" "--egress-allow-private" "${argv}"

# 2. The opt-in adds the flag, leaving the rest of the mapping intact.
argv=$(run OBR_TOKEN=t OBR_EGRESS_ALLOW_PRIVATE=1 --)
expect_arg "opt-in adds the flag" "--egress-allow-private" "${argv}"
expect_arg "opt-in keeps the token" "t" "${argv}"

# 3. Every accepted spelling.
for v in true TRUE yes on; do
  argv=$(run OBR_EGRESS_ALLOW_PRIVATE="${v}" --)
  expect_arg "opt-in spelling ${v}" "--egress-allow-private" "${argv}"
done

# 4. Explicit off values and junk keep the deny, and junk warns on stderr.
for v in 0 false no off; do
  argv=$(run OBR_EGRESS_ALLOW_PRIVATE="${v}" --)
  expect_no_arg "off value ${v}" "--egress-allow-private" "${argv}"
done
argv=$(run OBR_EGRESS_ALLOW_PRIVATE=yess)
expect_no_arg "unrecognised value keeps the deny" "--egress-allow-private" "${argv}"
grep -q "is not 1/true/yes/on" <(env PATH="${stub_dir}:${PATH}" OBR_EGRESS_ALLOW_PRIVATE=yess "${ENTRYPOINT}" 2>&1 >/dev/null) \
  || { echo "FAIL: unrecognised value did not warn" >&2; fail=1; }

# 5. Explicit argv is honoured verbatim and the OBR_* mapping is not applied --
#    the form the verification runs used to work around a discarding entrypoint.
argv=$(run OBR_TOKEN=from-env -- --bind 127.0.0.1 --rist-port 6000 --egress-allow-private)
expect_arg "argv bind is honoured" "127.0.0.1" "${argv}"
expect_arg "argv rist port is honoured" "6000" "${argv}"
expect_arg "argv flag is honoured" "--egress-allow-private" "${argv}"
expect_no_arg "argv form does not inject the default bind" "0.0.0.0" "${argv}"
expect_no_arg "argv form does not inject an env token" "from-env" "${argv}"
expect_no_arg "argv form does not inject --token" "--token" "${argv}"

if [ "${fail}" -ne 0 ]; then
  echo "entrypoint_args_test: FAILED" >&2
  exit 1
fi
echo "entrypoint_args_test: OK"
