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
# BASE_IMAGE is an override hook only. The default is the floating tag; the
# reproducible path pins the digest recorded in docker/base-image.lock.
ARG BASE_IMAGE=debian:sid
FROM ${BASE_IMAGE} AS build

# The source label links the GHCR package to this repository, so the package
# inherits the repository's visibility.
LABEL org.opencontainers.image.source=https://github.com/patcarter883/open-broadcast-receiver
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake meson ninja-build pkg-config git \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# No version relaxation: the floor is real now that the base meets it.
RUN cmake -S . -B build -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTING=OFF \
    && cmake --build build -j"$(nproc)"
# ristsrppasswd ships WITH the image. A relay's SRP verifier is generated inside
# the container, by the librist that will validate against it — the node agent is
# not a librist image and has no such tool (measured: relay_failed, "executable
# file not found in $PATH"). librist already builds its tools as part of the
# vendored dependency, so this is an install, not a build; the path is found
# rather than hardcoded because ExternalProject's layout differs between an
# in-repo build and this one.
RUN set -eux; tool="$(find /src -type f -name ristsrppasswd | head -1)"; \
    test -n "$tool"; install -m 0755 "$tool" /usr/local/bin/ristsrppasswd

FROM ${BASE_IMAGE} AS runtime
# libva2 + mesa-va-drivers are REQUIRED, not optional: the transcode tier prefers
# vah264enc/vah265enc/vaav1dec (see transcode_encoder_alternatives), and without
# the VAAPI *driver* those elements exist but fail to initialise -- the tier then
# falls back to x264enc/x265enc and runs on the CPU, which is the opposite of the
# intended deployment. The node agent passes the render node in alongside this.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav libgstreamer1.0-0 curl ca-certificates tini \
    libva2 libva-drm2 mesa-va-drivers fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -m obr
# Address-family policy: a name that answers both families must rank IPv4 first
# for every getaddrinfo() caller in this image that dials a NAME (rtmps keeps
# the hostname for TLS validation). The rtmp path does not rely on this file --
# the receiver pins a vetted A record itself -- but a node network carries no
# IPv6 route, so the policy is baked in rather than left to glibc's RFC 6724
# sort, which returns AAAA first whenever the container holds a
# routable-looking IPv6 address (measured: DECISIONS §20). Image layer, not a
# run-time flag, so every session gets it and no address is hardcoded.
RUN printf '%s\n' '# IPv4 first: this image runs on nodes without an IPv6 route.' \
                  'precedence ::ffff:0:0/96 100' > /etc/gai.conf
COPY --from=build /src/build/open-broadcast-receiver /usr/local/bin/
COPY --from=build /usr/local/bin/ristsrppasswd /usr/local/bin/
# The slate's wordmark. fonts-dejavu-core above is REQUIRED, not cosmetic:
# textoverlay renders nothing without a font, so the slate's caption would be
# invisible in an image that installed none. The placeholder lives in
# docker/standby/ (see its README); --standby-assets points here.
COPY docker/standby/logo.png /usr/local/share/open-broadcast/standby-logo.png
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
USER 10001:10001
EXPOSE 8080/tcp 5000/udp
# tini as PID 1 so signals reach the receiver for graceful /stop on SIGTERM.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
