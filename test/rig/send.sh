#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Pat Carter
#
# Rig sender: live H.264+AAC TS test stream → rist_feed → the bond hop.
# Env: DEST_HOST, DEST_PORT (the rist2rist input), BITRATE_KBPS (default 3000).
set -euo pipefail
: "${DEST_HOST:?}" "${DEST_PORT:?}"
BITRATE_KBPS="${BITRATE_KBPS:-3000}"
# SOURCE_CODEC selects what the stand-in encoder puts on the wire. AV1 is what the
# transcode tier exists for: it cannot be copied to RTMP, so an RTMP output must
# carry a transcode block, and the other outputs must still be able to copy it.
SOURCE_CODEC="${SOURCE_CODEC:-h264}"

if [ "${SOURCE_CODEC}" = "av1" ]; then
  # enable-custom-mappings=true is MANDATORY here: mpegtsmux will not accept
  # video/x-av1 using standard stream types, so without it the mux never
  # negotiates. The receiver's copy path is a tsparse passthrough and needs
  # nothing, but any site that MUXES AV1 into TS must set this.
  exec gst-launch-1.0 -q \
      videotestsrc is-live=true pattern=smpte \
        ! video/x-raw,width=1280,height=720,framerate=30/1 \
        # kbit/s, exactly like x264enc's `bitrate` below -- NOT bits/s. The two
        # GStreamer encoders agree on the unit, so they must agree on the value;
        # multiplying here asked libaom for 3 Tbps and it refused the stream.
        ! av1enc target-bitrate="${BITRATE_KBPS}" keyframe-max-dist=60 \
        ! av1parse \
        ! mpegtsmux name=m alignment=7 enable-custom-mappings=true ! fdsink fd=1 \
      audiotestsrc is-live=true wave=sine \
        ! audio/x-raw,rate=48000,channels=2 ! avenc_aac ! aacparse ! m. \
      | rist_feed "${DEST_HOST}" "${DEST_PORT}"
fi

exec gst-launch-1.0 -q \
    videotestsrc is-live=true pattern=smpte \
      ! video/x-raw,width=1280,height=720,framerate=30/1 \
      ! x264enc tune=zerolatency bitrate="${BITRATE_KBPS}" key-int-max=60 \
      ! h264parse config-interval=1 \
      ! mpegtsmux name=m alignment=7 ! fdsink fd=1 \
    audiotestsrc is-live=true wave=sine \
      ! audio/x-raw,rate=48000,channels=2 ! avenc_aac ! aacparse ! m. \
    | rist_feed "${DEST_HOST}" "${DEST_PORT}"
