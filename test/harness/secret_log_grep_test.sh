#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# The SRP password never reaches a log line. This is the gate that keeps the
# claim true as the tree changes, rather than a property of the one review that
# checked it.
#
# Two rules over every log sink in source/ (`log(...)`, `stderr_log(...)`,
# `say(...)`, std::printf/fprintf/cerr/cout):
#
#   1. No sink may name a credential: srp_password, key_or_streamid (the SRT
#      streamid, which is the platform's stream key), relay_secret, psk.
#   2. No sink may print a URL variable unless it goes through
#      loggable_endpoint() — a RIST URL's query carries `username=` and
#      `password=`, and scheme+host:port is all a log line needs.
#
# Statements are flattened first: a log call wrapped over several lines is still
# one statement, and a per-line grep would miss exactly the case that matters
# (the connect-mode endpoint log is a three-line ternary).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
repo="${1:-${root}}"

flat="$(mktemp)"
trap 'rm -f "${flat}"' EXIT

files=0
while IFS= read -r -d '' file; do
  files=$((files + 1))
  perl -0pe 's/\n\s+/ /g; s/;\s*/;\n/g' "${file}" >> "${flat}"
done < <(find "${repo}/source" -type f \( -name '*.cpp' -o -name '*.h' \) -print0)

sink='(^|[^A-Za-z0-9_:])(log|stderr_log|say|std::printf|std::fprintf|std::cerr|std::cout|std::puts)[[:space:]]*\('

scanned=$(grep -cE "${sink}" "${flat}" || true)
# A test that scans nothing passes trivially. The tree has ~120 such statements;
# the floor is deliberately well below that so ordinary edits do not trip it.
if [[ "${scanned}" -lt 60 ]]; then
  echo "FAIL: only ${scanned} log statements found in ${files} files under ${repo}/source" >&2
  exit 1
fi

fail=0

if hits=$(grep -nE "${sink}" "${flat}" \
    | grep -E 'srp_password|key_or_streamid|relay_secret|\bpsk\b'); then
  echo "FAIL: a log statement names a credential:" >&2
  echo "${hits}" >&2
  fail=1
fi

if hits=$(grep -nE "${sink}" "${flat}" \
    | grep -E '\burl\b|_url\b' \
    | grep -vE "loggable_endpoint\(|find\('\\?'\)" \
    | grep -v "Usage:"); then
  echo "FAIL: a log statement prints a URL without loggable_endpoint():" >&2
  echo "${hits}" >&2
  fail=1
fi

if [[ "${fail}" -ne 0 ]]; then
  exit 1
fi

echo "secret_log_grep: ${scanned} log statements in ${files} files carry no credential"
