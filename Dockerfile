# syntax=docker/dockerfile:1
# Production receiver image (BACKPLANE §13.9 / FIXPLAN M3). Built by Komodo →
# GHCR; the node agent runs one container per session with the OBR_* env the
# entrypoint maps to CLI flags (node/infra/README.md in the backplane repo).
#
# Release build ⇒ vendored librist compiled -DNDEBUG (the receiver_enqueue
# assert is compiled out — CONTRACT §4).
#
# The base must provide GStreamer >= 1.28, which is the source's own floor, and
# `debian:sid` is the apt-based way to get it (1.28.7). The previous ubuntu:24.04
# base shipped 1.24 and the floor was sed-relaxed to match — which meant running
# production on a GStreamer the source does not support, and silently losing every
# capability that arrived after 1.24. The one that bites here: mpegtsmux gained
# `enable-custom-mappings`, without which AV1 cannot be muxed into MPEG-TS at all
# ("custom mappings for which there are no official specifications"). A relaxed
# floor did not merely tolerate an old version, it hid the gap.
FROM debian:sid AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake meson ninja-build pkg-config git \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# No version relaxation: the floor is real now that the base meets it.
RUN cmake -S . -B build -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTING=OFF \
    && cmake --build build -j"$(nproc)"

FROM debian:sid
# libva2 + mesa-va-drivers are REQUIRED, not optional: the transcode tier prefers
# vah264enc/vah265enc/vaav1dec (see transcode_encoder_alternatives), and without
# the VAAPI *driver* those elements exist but fail to initialise -- the tier then
# falls back to x264enc/x265enc and runs on the CPU, which is the opposite of the
# intended deployment. The node agent passes the render node in alongside this.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav libgstreamer1.0-0 curl ca-certificates tini \
    libva2 libva-drm2 mesa-va-drivers \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -m obr
COPY --from=build /src/build/open-broadcast-receiver /usr/local/bin/
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
USER 10001:10001
EXPOSE 8080/tcp 5000/udp
# tini as PID 1 so signals reach the receiver for graceful /stop on SIGTERM.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
