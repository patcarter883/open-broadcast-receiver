#!/usr/bin/env bash
# build-appimage.sh
#
# Build the receiver binary and package it as an AppImage.
# GStreamer must be installed on the target server separately.
#
# Usage (from repo root):
#   bash packaging/build-appimage.sh
#
# Optional env overrides:
#   BUILD_TYPE   CMake build type (default: Release)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
APPDIR="$REPO_ROOT/AppDir"
TOOLS_DIR="$REPO_ROOT/packaging/tools"
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc)"

export PATH="$TOOLS_DIR:$PATH"

# linuxdeploy bundles an old strip binary that cannot handle SHT_RELR sections
# (.relr.dyn) produced by Arch/CachyOS toolchains.
export NO_STRIP=1

# ---------------------------------------------------------------------------
# 1. Build receiver binary
# ---------------------------------------------------------------------------

echo "Configuring (${BUILD_TYPE})..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_C_COMPILER=gcc-15 \
    -DCMAKE_CXX_COMPILER=g++-15 \
    -DCMAKE_CXX_FLAGS="-march=x86-64" \
    -DCMAKE_C_FLAGS="-march=x86-64"

echo "Building..."
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# ---------------------------------------------------------------------------
# 2. Patch GNU_PROPERTY_X86_ISA_1_NEEDED back to x86-64-baseline
#
#    glibc on CachyOS sets a high ISA note even though it uses runtime IFUNC
#    dispatch.  Patching the note lets the kernel load the binary on any CPU.
# ---------------------------------------------------------------------------

echo "Patching ISA note..."
python3 - "$BUILD_DIR/open-broadcast-receiver" << 'PYEOF'
import sys, subprocess, tempfile, os
binary = sys.argv[1]
tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.note')
tmp.close()
try:
    subprocess.run(['objcopy', '--dump-section', f'.note.gnu.property={tmp.name}', binary], check=True)
    with open(tmp.name, 'rb') as f:
        data = bytearray(f.read())
    prop_type = bytes([0x02, 0x80, 0x00, 0xc0])
    idx = data.find(prop_type)
    if idx >= 0:
        before = int.from_bytes(data[idx+8:idx+12], 'little')
        data[idx+8:idx+12] = (1).to_bytes(4, 'little')
        with open(tmp.name, 'wb') as f:
            f.write(data)
        subprocess.run(['objcopy', '--update-section', f'.note.gnu.property={tmp.name}', binary], check=True)
        print(f"  Patched ISA note: 0x{before:02x} -> 0x01 (x86-64-baseline)")
    else:
        print("  No GNU_PROPERTY_X86_ISA_1_NEEDED note found, nothing to patch")
finally:
    os.unlink(tmp.name)
PYEOF

# ---------------------------------------------------------------------------
# 3. Build AppImage
# ---------------------------------------------------------------------------

echo "Building AppImage..."

rm -rf "${APPDIR:?}/usr" "${APPDIR:?}/AppRun" "${APPDIR:?}/AppRun.wrapped"
rm -rf "${APPDIR:?}/apprun-hooks"
rm -f  "${APPDIR:?}"/*.desktop "${APPDIR:?}"/*.png

export LINUXDEPLOY
# Exclude all system libs — the target server (Ubuntu 26.04) has GStreamer 1.28
# and GLib 2.88 installed via apt. Bundling the CachyOS-compiled versions causes
# SIGILL because CachyOS builds with AVX-512 which Broadwell servers don't have.
EXCLUDE_LIBS=(
    libglib-2.0.so.0
    libgobject-2.0.so.0
    libgmodule-2.0.so.0
    libgstreamer-1.0.so.0
    libgstapp-1.0.so.0
    libgstbase-1.0.so.0
    libffi.so.8
    libpcre2-8.so.0
    liblzma.so.5
    libzstd.so.1
    libbz2.so.1.0
    libunwind.so.8
    libdw.so.1
    libelf.so.1
)
EXCLUDE_ARGS=()
for lib in "${EXCLUDE_LIBS[@]}"; do
    EXCLUDE_ARGS+=(--exclude-library "$lib")
done

"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --executable "$BUILD_DIR/open-broadcast-receiver" \
    --desktop-file "$REPO_ROOT/packaging/open-broadcast-receiver.desktop" \
    --icon-file "$REPO_ROOT/packaging/open-broadcast-receiver.png" \
    "${EXCLUDE_ARGS[@]}" \
    --output appimage

APPIMAGE=$(find "$REPO_ROOT" -maxdepth 1 -name "open-broadcast-receiver*.AppImage" \
           -newer "$BUILD_DIR/open-broadcast-receiver" | head -1)

[[ -n "$APPIMAGE" ]] || { echo "ERROR: AppImage not found after linuxdeploy run."; exit 1; }

SIZE=$(ls -lh "$APPIMAGE" | awk '{print $5}')
echo ""
echo "Done: $APPIMAGE ($SIZE)"
echo ""
echo "To deploy:"
echo "  scp $APPIMAGE user@server:"
echo "  ssh user@server 'bash -s' < packaging/install-ubuntu.sh"
