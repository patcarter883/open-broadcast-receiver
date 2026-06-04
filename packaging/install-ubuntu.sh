#!/usr/bin/env bash
# install-ubuntu.sh
#
# Deploy open-broadcast-receiver on Ubuntu 26.04.
#
# The AppImage is extracted once at install time so the service runs the binary
# directly — no FUSE required at runtime.
#
# Usage:
#   sudo bash install-ubuntu.sh [--appimage <path>] [--port <port>] [--token <token>]
#                               [--install-dir <dir>] [--no-service]
#
# Defaults:
#   --appimage    ./open-broadcast-receiver*.AppImage  (found in current dir)
#   --port        8080
#   --install-dir /opt/open-broadcast-receiver
#   --token       (required; no default)

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

APPIMAGE=""
CONTROL_PORT="8080"
TOKEN=""
INSTALL_DIR="/opt/open-broadcast-receiver"
CREATE_SERVICE=1
SERVICE_NAME="open-broadcast-receiver"
SERVICE_USER="obr"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --appimage)    APPIMAGE="$2";     shift 2 ;;
        --port)        CONTROL_PORT="$2"; shift 2 ;;
        --token)       TOKEN="$2";        shift 2 ;;
        --install-dir) INSTALL_DIR="$2";  shift 2 ;;
        --no-service)  CREATE_SERVICE=0;  shift   ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

info() { echo "[INFO]  $*"; }
die()  { echo "[ERROR] $*" >&2; exit 1; }

require_root() {
    [[ $EUID -eq 0 ]] || die "This script must be run as root (sudo)."
}

# ---------------------------------------------------------------------------
# 1. Find the AppImage
# ---------------------------------------------------------------------------

find_appimage() {
    if [[ -n "$APPIMAGE" ]]; then
        [[ -f "$APPIMAGE" ]] || die "AppImage not found: $APPIMAGE"
        return
    fi
    local found
    found=$(find . -maxdepth 1 -name "open-broadcast-receiver*.AppImage" | head -1)
    [[ -n "$found" ]] || die "No AppImage found in current directory. Pass --appimage <path>."
    APPIMAGE="$found"
}

# ---------------------------------------------------------------------------
# 2. Extract the AppImage into INSTALL_DIR
#    This avoids any FUSE dependency at runtime — the service runs AppRun
#    directly from the extracted tree.
# ---------------------------------------------------------------------------

extract_appimage() {
    info "Extracting $APPIMAGE -> $INSTALL_DIR ..."
    rm -rf "$INSTALL_DIR"
    mkdir -p "$(dirname "$INSTALL_DIR")"

    # --appimage-extract always writes to ./squashfs-root; move it afterwards
    local tmp_extract
    tmp_extract=$(mktemp -d)
    chmod +x "$APPIMAGE"
    (cd "$tmp_extract" && "$APPIMAGE" --appimage-extract > /dev/null)
    mv "$tmp_extract/squashfs-root" "$INSTALL_DIR"
    rmdir "$tmp_extract"

    # AppRun is the entry point — it sources apprun-hooks/ which set
    # GST_PLUGIN_SYSTEM_PATH_1_0 and related vars before exec-ing the binary.
    APPRUN="${INSTALL_DIR}/AppRun"
    [[ -x "$APPRUN" ]] || die "AppRun not found after extraction: $APPRUN"
    info "Extracted to $INSTALL_DIR"
}

# ---------------------------------------------------------------------------
# 3. Create a dedicated system user
# ---------------------------------------------------------------------------

create_service_user() {
    if ! id -u "$SERVICE_USER" &>/dev/null; then
        info "Creating system user '$SERVICE_USER'..."
        useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    fi
}

# ---------------------------------------------------------------------------
# 4. Write systemd unit
# ---------------------------------------------------------------------------

write_systemd_service() {
    local unit_file="/etc/systemd/system/${SERVICE_NAME}.service"
    local env_file="/etc/default/${SERVICE_NAME}"

    # Write env file so the token is not visible in ps output
    if [[ ! -f "$env_file" ]]; then
        info "Writing environment file: $env_file"
        cat > "$env_file" <<EOF
# open-broadcast-receiver configuration
# Edit this file then: systemctl restart ${SERVICE_NAME}
OBR_TOKEN=${TOKEN}
OBR_PORT=${CONTROL_PORT}
EOF
        chmod 640 "$env_file"
    else
        info "Environment file $env_file already exists — not overwriting."
        info "  Edit it manually to change OBR_TOKEN / OBR_PORT."
    fi

    info "Writing systemd unit: $unit_file"
    cat > "$unit_file" <<EOF
[Unit]
Description=Open Broadcast Receiver (RIST receiver / restreamer)
After=network-online.target
Wants=network-online.target

[Service]
EnvironmentFile=/etc/default/${SERVICE_NAME}
ExecStart=${APPRUN} --port \${OBR_PORT} --token \${OBR_TOKEN}
Restart=on-failure
RestartSec=5
User=${SERVICE_USER}

# Hardening
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/tmp

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable "${SERVICE_NAME}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

require_root
find_appimage

# resolve to absolute path before any cd
APPIMAGE="$(realpath "$APPIMAGE")"

info "=== open-broadcast-receiver installer (Ubuntu 26.04) ==="
info "  AppImage: $APPIMAGE"
info "  Install:  $INSTALL_DIR"
info "  Port:     $CONTROL_PORT"
info "  Service:  $([[ $CREATE_SERVICE -eq 1 ]] && echo yes || echo no)"

extract_appimage

if [[ $CREATE_SERVICE -eq 1 ]]; then
    [[ -n "$TOKEN" ]] || die "A --token <secret> is required when creating the systemd service."
    create_service_user
    write_systemd_service

    info ""
    info "=== Installed. ==="
    info ""
    info "Start now:  systemctl start ${SERVICE_NAME}"
    info "Status:     systemctl status ${SERVICE_NAME}"
    info "Logs:       journalctl -u ${SERVICE_NAME} -f"
    info "Stop:       systemctl stop ${SERVICE_NAME}"
    info ""
    info "Token and port are in /etc/default/${SERVICE_NAME}"
else
    info ""
    info "=== Installed. ==="
    info ""
    info "Run manually:"
    info "  ${APPRUN} --port ${CONTROL_PORT} --token <secret>"
fi
