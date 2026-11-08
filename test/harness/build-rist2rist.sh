#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Build test/rig/rist2rist-tool.c (the production bridge source) for a local
# rig, against the system libRIST. The router image builds the same file inside
# test/rig/Dockerfile.bond.
#
# Usage: test/harness/build-rist2rist.sh <librist-src> <librist-build> [output]
#   <librist-src>    libRIST source tree containing tools/ and contrib/
#   <librist-build>  a configured meson build directory for that tree
#                    (supplies config.h and vcs_version.h)
#   [output]         default build/rig/rist2rist
#
# Exit: 0 built, 1 usage/build failure.

set -euo pipefail

src="${1:-}"
build="${2:-}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${3:-$root/build/rig/rist2rist}"

if [[ -z "$src" || -z "$build" ]]; then
  echo "usage: $0 <librist-src> <librist-build> [output]" >&2
  exit 1
fi
for f in "$src/tools/oob_shared.c" "$src/tools/string-shim.h" \
         "$build/config.h" "$build/include/vcs_version.h"; do
  [[ -f "$f" ]] || { echo "FAIL: missing $f" >&2; exit 1; }
done

# Prometheus support is a libRIST build option; the system librist here is
# built without it, so compile the tool with the same answer rather than
# referencing symbols that are not there.
inc="$(mktemp -d)"
trap 'rm -rf "$inc"' EXIT
sed 's/#define HAVE_PROMETHEUS_SUPPORT 1/#define HAVE_PROMETHEUS_SUPPORT 0/' \
  "$build/config.h" >"$inc/config.h"
cp "$build/include/vcs_version.h" "$inc/"

mkdir -p "$(dirname "$out")"
gcc -std=gnu11 -O2 -o "$out" \
  "$root/test/rig/rist2rist-tool.c" \
  "$src/tools/oob_shared.c" "$src/tools/srp_shared.c" \
  -I"$inc" -I"$src/include" -I"$src/tools" -I"$src/contrib" \
  -I"$build/include" -I"$build/include/librist" \
  -lrist -lmbedcrypto

echo "built $out"
