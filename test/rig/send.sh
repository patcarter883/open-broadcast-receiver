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
  # target-bitrate is kbit/s, exactly like x264enc's `bitrate` below -- NOT bits/s.
  # The two encoders agree on the unit, so they must agree on the value;
  # multiplying here asked the encoder for 3 Tbps and it refused the stream.
  # (Keep commentary OUT of the continued command: a comment there ends the
  # pipeline early and gst-launch reports a bogus "no element video".)
  #
  # vaav1enc: HARDWARE AV1, matching production (software encoding is not used in
  # production). This also removes a trap -- libaom's av1enc defaults to cpu-used=0
  # and encodes 720p at under 1 fps, which starves the stream: the receiver's ring
  # then holds a video trickle, tsdemux never gets enough AV1 to map the
  # stream_type 0x06 entry, and every transcode output starves while the copy path
  # (tsparse passthrough, no demux) looks perfectly healthy. A live AV1 source must
  # use a realtime-capable encoder -- and the sender therefore needs the GPU. The
  # property is bitrate (kbps), NOT target-bitrate: that name exists on the software
  # encoders and does not exist on vaav1enc.
  exec gst-launch-1.0 -q \
      videotestsrc is-live=true pattern=smpte \
        ! video/x-raw,width=1280,height=720,framerate=30/1 \
        ! vaav1enc bitrate="${BITRATE_KBPS}" key-int-max=60 \
        ! av1parse \
        ! mpegtsmux name=m alignment=7 enable-custom-mappings=true ! fdsink fd=1 \
      audiotestsrc is-live=true wave=sine \
        ! audio/x-raw,rate=48000,channels=2 ! avenc_aac ! aacparse ! m. \
      | rist_feed "${DEST_HOST}" "${DEST_PORT}"
fi

exec gst-launch-1.0 -q \
    videotestsrc is-live=true pattern=smpte \
      ! video/x-raw,width=1280,height=720,framerate=30/1 \
      ! vah264enc bitrate="${BITRATE_KBPS}" key-int-max=60 \
      ! h264parse config-interval=1 \
      ! mpegtsmux name=m alignment=7 ! fdsink fd=1 \
    audiotestsrc is-live=true wave=sine \
      ! audio/x-raw,rate=48000,channels=2 ! avenc_aac ! aacparse ! m. \
    | rist_feed "${DEST_HOST}" "${DEST_PORT}"
