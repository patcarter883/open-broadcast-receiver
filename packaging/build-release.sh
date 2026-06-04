#!/usr/bin/env bash
# Build a stripped release binary that requires only GStreamer on the target server.
#
# Usage:
#   cd <repo-root>
#   bash packaging/build-release.sh
#
# Output: ./dist/open-broadcast-receiver
#
# Required on the server before running:
#   Ubuntu/Debian:
#     apt install gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
#                 gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
#   RHEL/Fedora:
#     dnf install gstreamer1-plugins-base gstreamer1-plugins-good \
#                 gstreamer1-plugins-bad-free gstreamer1-plugins-ugly-free gstreamer1-libav

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
DIST_DIR="$REPO_ROOT/dist"

echo "Configuring..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-march=x86-64" \
    -DCMAKE_C_FLAGS="-march=x86-64"

echo "Building..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/open-broadcast-receiver" "$DIST_DIR/open-broadcast-receiver"
strip "$DIST_DIR/open-broadcast-receiver"

# On distros that build glibc with AVX-512 (CachyOS, Arch with performance packages),
# the linker inherits glibc's ISA requirements into the final binary even though glibc
# uses runtime IFUNC dispatch and works on any x86-64 CPU. Patch the
# GNU_PROPERTY_X86_ISA_1_NEEDED note back to x86-64-baseline so the binary will run
# on any x86-64 cloud server without hitting "CPU ISA level is lower than required".
python3 - "$DIST_DIR/open-broadcast-receiver" << 'PYEOF'
import sys, subprocess, tempfile, os
binary = sys.argv[1]
tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.note')
tmp.close()
try:
    subprocess.run(['objcopy', '--dump-section', f'.note.gnu.property={tmp.name}', binary], check=True)
    with open(tmp.name, 'rb') as f:
        data = bytearray(f.read())
    # GNU_PROPERTY_X86_ISA_1_NEEDED stored as little-endian 0xc0008002
    prop_type = bytes([0x02, 0x80, 0x00, 0xc0])
    idx = data.find(prop_type)
    if idx >= 0:
        before = int.from_bytes(data[idx+8:idx+12], 'little')
        data[idx+8:idx+12] = (1).to_bytes(4, 'little')  # baseline only
        with open(tmp.name, 'wb') as f:
            f.write(data)
        subprocess.run(['objcopy', '--update-section', f'.note.gnu.property={tmp.name}', binary], check=True)
        print(f"Patched ISA note: 0x{before:02x} -> 0x01 (x86-64-baseline)")
    else:
        print("No GNU_PROPERTY_X86_ISA_1_NEEDED note found, nothing to patch")
finally:
    os.unlink(tmp.name)
PYEOF

SIZE=$(ls -lh "$DIST_DIR/open-broadcast-receiver" | awk '{print $5}')
echo ""
echo "Done: $DIST_DIR/open-broadcast-receiver ($SIZE)"
echo ""
echo "To deploy:"
echo "  scp dist/open-broadcast-receiver user@server:"
echo "  ssh user@server './open-broadcast-receiver --port 8080 --token <secret>'"
