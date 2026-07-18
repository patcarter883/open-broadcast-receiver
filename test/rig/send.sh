#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Rig sender: live H.264+AAC TS test stream → rist_feed → the bond hop.
# Env: DEST_HOST, DEST_PORT (the rist2rist input), BITRATE_KBPS (default 3000).
set -euo pipefail
: "${DEST_HOST:?}" "${DEST_PORT:?}"
BITRATE_KBPS="${BITRATE_KBPS:-3000}"

exec gst-launch-1.0 -q \
  videotestsrc is-live=true pattern=smpte \
    ! video/x-raw,width=1280,height=720,framerate=30/1 \
    ! x264enc tune=zerolatency bitrate="${BITRATE_KBPS}" key-int-max=60 \
    ! h264parse config-interval=1 \
    ! mpegtsmux name=m alignment=7 ! fdsink fd=1 \
  audiotestsrc is-live=true wave=sine \
    ! audio/x-raw,rate=48000,channels=2 ! avenc_aac ! aacparse ! m. \
  | rist_feed "${DEST_HOST}" "${DEST_PORT}"
