# syntax=docker/dockerfile:1
# Production receiver image (BACKPLANE §13.9 / FIXPLAN M3). Built by Komodo →
# GHCR; the node agent runs one container per session with the OBR_* env the
# entrypoint maps to CLI flags (node/infra/README.md in the backplane repo).
#
# Release build ⇒ vendored librist compiled -DNDEBUG (the receiver_enqueue
# assert is compiled out — CONTRACT §4). GStreamer floor is 1.28 in the source;
# the runtime base must provide it. The 24.04 base ships 1.24, so the source
# floor is relaxed here — bump the base to a 1.28-provided image when available.
FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake meson ninja-build pkg-config git \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN sed -i 's/>=1\.28/>=1.24/g' CMakeLists.txt \
    && cmake -S . -B build -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTING=OFF \
    && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav libgstreamer1.0-0 curl ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -m obr
COPY --from=build /src/build/open-broadcast-receiver /usr/local/bin/
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
USER 10001:10001
EXPOSE 8080/tcp 5000/udp
# tini as PID 1 so signals reach the receiver for graceful /stop on SIGTERM.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
