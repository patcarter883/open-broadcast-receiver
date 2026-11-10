#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# entrypoint_args_test.sh — asserts the shipped docker/entrypoint.sh honours an
# explicit argv, maps OBR_EGRESS_ALLOW_PRIVATE to --egress-allow-private
# without weakening the default deny, and maps the BYON trunk variables
# (OBR_TRUNK_URL for connect-mode ingest; OBR_RELAY_IN/OBR_RELAY_OUT plus
# OBR_TRUNK_SRP_FILE/OBR_TRUNK_SECRET/OBR_TRUNK_AES for relay mode).
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

# 6. Relay mode: both legs map, and none of the receiver-only flags ride along.
#    A relay takes no control port, no token and no bind, and the receiver
#    binary rejects them outright -- so their absence here is the assertion.
argv=$(run OBR_RELAY_IN='rist://@[::]:5000' OBR_RELAY_OUT='rist://@[::]:6010' \
           OBR_TRUNK_SRP_FILE=/run/obr/srp.txt OBR_TRUNK_SECRET=s3cret \
           OBR_TRUNK_AES=128 --)
expect_arg "relay in leg" "--relay-in" "${argv}"
expect_arg "relay in leg value" "rist://@[::]:5000" "${argv}"
expect_arg "relay out leg" "--relay-out" "${argv}"
expect_arg "relay out leg value" "rist://@[::]:6010" "${argv}"
expect_arg "relay srp file" "--relay-srp" "${argv}"
expect_arg "relay srp path" "/run/obr/srp.txt" "${argv}"
expect_arg "relay secret" "--relay-secret" "${argv}"
expect_arg "relay secret value" "s3cret" "${argv}"
expect_arg "relay aes" "--relay-aes" "${argv}"
expect_arg "relay aes value" "128" "${argv}"
expect_no_arg "relay takes no control bind" "--bind" "${argv}"
expect_no_arg "relay takes no rist port" "--rist-port" "${argv}"
expect_no_arg "relay takes no token" "--token" "${argv}"
expect_no_arg "relay takes no standby flag" "--standby-slate" "${argv}"

# 7. Relay mode with only one leg is refused before exec: a single leg would
#    otherwise start a packet pump with nowhere to pump to.
if env PATH="${stub_dir}:${PATH}" OBR_RELAY_IN='rist://@[::]:5000' "${ENTRYPOINT}" >/dev/null 2>&1; then
  echo "FAIL: relay mode with one leg must fail" >&2
  fail=1
fi
grep -q "needs BOTH OBR_RELAY_IN and OBR_RELAY_OUT" \
  <(env PATH="${stub_dir}:${PATH}" OBR_RELAY_OUT='rist://@[::]:6010' "${ENTRYPOINT}" 2>&1 >/dev/null) \
  || { echo "FAIL: one-legged relay did not explain itself" >&2; fail=1; }

# 8. Trunk (connect-mode) ingest: dial, and never pass --rist-port. The control
#    plane is unchanged, so --bind and --token must survive.
argv=$(run OBR_TRUNK_URL='rist://relay.example.net:6010' OBR_TOKEN=t --)
expect_arg "trunk connect url flag" "--connect-url" "${argv}"
expect_arg "trunk connect url value" "rist://relay.example.net:6010" "${argv}"
expect_no_arg "trunk mode binds no RIST port" "--rist-port" "${argv}"
expect_arg "trunk mode keeps the control bind" "--bind" "${argv}"
expect_arg "trunk mode keeps the token" "t" "${argv}"

# 8b. Trunk SRP pair: both map; one alone is refused (the receiver rejects half
#     a pair anyway, so failing here keeps the message useful).
argv=$(run OBR_TRUNK_URL='rist://relay.example.net:6010' \
           OBR_TRUNK_SRP_USER=venue OBR_TRUNK_SRP_PASS=hunter2 OBR_TOKEN=t --)
expect_arg "trunk srp user flag" "--srp-user" "${argv}"
expect_arg "trunk srp user value" "venue" "${argv}"
expect_arg "trunk srp pass flag" "--srp-pass" "${argv}"
expect_arg "trunk srp pass value" "hunter2" "${argv}"
if env PATH="${stub_dir}:${PATH}" OBR_TRUNK_URL='rist://relay.example.net:6010' \
        OBR_TRUNK_SRP_USER=venue "${ENTRYPOINT}" >/dev/null 2>&1; then
  echo "FAIL: half an SRP pair must fail in the entrypoint" >&2
  fail=1
fi
# The listen form must never see SRP client flags: it authenticates callers.
argv=$(run OBR_TOKEN=t OBR_TRUNK_SRP_USER=venue OBR_TRUNK_SRP_PASS=hunter2 --)
expect_no_arg "listen form takes no --srp-user" "--srp-user" "${argv}"
expect_no_arg "listen form takes no --srp-pass" "--srp-pass" "${argv}"

# 9. The listen form is unchanged when no trunk variable is set.
argv=$(run OBR_TOKEN=t --)
expect_no_arg "listen form is not connect mode" "--connect-url" "${argv}"
expect_arg "listen form still passes the port" "5000" "${argv}"

if [ "${fail}" -ne 0 ]; then
  echo "entrypoint_args_test: FAILED" >&2
  exit 1
fi
echo "entrypoint_args_test: OK"
