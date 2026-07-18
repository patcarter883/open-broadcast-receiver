# open-broadcast-receiver

Headless RIST **receiver / restreamer** — the partner application to
[`open-broadcast-encoder`](../open-broadcast-encoder). It receives the
encoder's RIST stream, then for **one or more** destinations either **copies**
(passthrough) or **reencodes** the video/audio and restreams to **RTMP / SRT /
RIST**. It runs on headless / virtualised servers and has **no UI** — all
control comes from the encoder over a small REST API.

This is a ground-up redevelopment of the original `ndi-rist-server`. The design
rationale and the full wire contract live in:

- [`DECISIONS.md`](DECISIONS.md) — every significant design decision + rejected alternatives.
- [`docs/CONTRACT.md`](docs/CONTRACT.md) — the encoder ⇄ receiver control & telemetry contract (the shared source of truth).
- [`docs/GSTREAMER.md`](docs/GSTREAMER.md) — verified GStreamer pipeline templates.
- [`docs/IMPL_PLAN.md`](docs/IMPL_PLAN.md) — file-by-file implementation plan.

## How it fits together

```
 open-broadcast-encoder                         open-broadcast-receiver (this app)
 ┌────────────────────┐   RIST/UDP (MPEG-TS)    ┌──────────────────────────────────┐
 │ NDI/SDP/MPEGTS in   │ ───────────────────────▶│ RISTNetReceiver (listen, ADVANCED)│
 │ encode → mpegtsmux  │                         │   → appsrc → tsdemux              │
 │ RISTNetSender(caller)│◀── RIST OOB telemetry ──│   → [copy | reencode] → tee       │
 │ HTTP control client │   (5-byte wan_telemetry)│   → RTMP / SRT / RIST outputs     │
 └─────────┬───────────┘                         └───────────────▲──────────────────┘
           │  HTTP/JSON control (Bearer token)                   │
           └──── POST /start · POST /stop · GET /status ──────────┘
```

- **Media** stays on RIST/UDP exactly as the encoder produces it.
- **Control** is REST/JSON over HTTP (cpp-httplib + nlohmann/json), Bearer-token
  authenticated. See `docs/CONTRACT.md`.
- **Telemetry** flows back to the encoder over the RIST OOB channel as the
  encoder's existing 5-byte `wan_telemetry` struct, and is also mirrored in
  `GET /status`.

## Build

Requirements (verified versions): CMake ≥ 3.28, a C++20 compiler (gcc 16 OK),
GStreamer ≥ 1.28, `nlohmann-json` (system package). `cpp-httplib` is vendored
(`external/httplib.h`). `rist-cpp` (with librist) is vendored under
`external/rist-cpp` and built once via ExternalProject into `build-external/`.

```sh
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release
cmake --build build
```

The hardware encoder/decoder elements (NVENC/AMF/QSV, `rav1enc`) are only
required on hosts that are asked to **reencode** with those families; the
receiver validates element availability at `POST /start` and returns a
`400 encoder_unavailable` rather than launching a broken pipeline. Copy
(passthrough) and software (x264/x265) reencode work on any host with the base
GStreamer plugins.

## Run

```sh
./build/open-broadcast-receiver \
    --control-port 8080 \
    --rist-port 5000 \
    --token "$RECEIVER_TOKEN"
```

It then idles until the encoder (or `curl`) issues `POST /start`. Example:

```sh
curl -s -X POST http://127.0.0.1:8080/start \
  -H "Authorization: Bearer $RECEIVER_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{
        "schema_version": 1,
        "session_id": "demo-1",
        "source": { "codec": "h264" },
        "outputs": [
          { "id": "cdn", "type": "rtmp", "url": "rtmp://a.rtmp.example/live",
            "key_or_streamid": "STREAMKEY",
            "video": { "mode": "copy", "codec": "h264" },
            "audio": { "mode": "copy", "codec": "aac" } }
        ]
      }'

curl -s http://127.0.0.1:8080/status -H "Authorization: Bearer $RECEIVER_TOKEN"
curl -s -X POST http://127.0.0.1:8080/stop -H "Authorization: Bearer $RECEIVER_TOKEN" -d '{"schema_version":1}'
```

## Security

- **Every** route requires `Authorization: Bearer <token>` (constant-time
  compared). Run with a long random `--token`; an empty token is an explicit
  dev/no-auth mode and must not be used in production.
- Bind to a private/management interface, or expose `0.0.0.0` only behind a
  source-IP allowlist / WireGuard. For public hops, build cpp-httplib with TLS.
- Destination URLs and stream keys are validated and never echoed back in
  `GET /status` or logs.

See `DECISIONS.md` §9 for the full security rationale.

## Licensing

Licensed under the **GNU Affero General Public License v3.0 or later**
(AGPL-3.0-or-later). See [`LICENSE`](LICENSE).

Third-party components retain their own licenses — see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Contributions are accepted under the same license with a DCO sign-off — see
[`CONTRIBUTING.md`](CONTRIBUTING.md).
