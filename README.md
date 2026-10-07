# open-broadcast-receiver

Headless RIST **receiver / restreamer** — the partner application to
[`open-broadcast-encoder`](../open-broadcast-encoder). It terminates the
encoder's RIST stream and fans it out, **copy-only** (H.264+AAC, no decode, no
re-encode, no GPU), to up to 8 **RTMP / RTMPS / SRT / RIST** destinations
simultaneously — one independent pipeline per output over an in-process ring,
so one flapping destination can never disturb the others. It runs on headless /
virtualised servers and has **no UI** — all control comes from the encoder over
a small REST API.

This is a ground-up redevelopment of the original `ndi-rist-server`. The design
rationale and the full wire contract live in:

- [`DECISIONS.md`](DECISIONS.md) — every significant design decision + rejected alternatives.
- [`docs/CONTRACT.md`](docs/CONTRACT.md) — the encoder ⇄ receiver control & telemetry contract (schema_version 3, the shared source of truth).
- [`docs/GSTREAMER.md`](docs/GSTREAMER.md) — verified fan-out pipeline templates.

## How it fits together

```
 open-broadcast-encoder                          open-broadcast-receiver (this app)
 ┌────────────────────┐   RIST/UDP (MPEG-TS)     ┌────────────────────────────────────┐
 │ NDI/SDP/MPEGTS in   │ ───────────────────────▶│ RISTNetReceiver (listen, ADVANCED, │
 │ encode → mpegtsmux  │  (bonded: multiple peers │  timing-mode=0, optional PSK)      │
 │ RISTNetSender(caller)│   on ONE port)          │   → SPMC ring ─┬─▶ RTMP pipeline   │
 │ HTTP control client │◀── RIST OOB telemetry ── │                ├─▶ SRT pipeline    │
 └─────────┬───────────┘  (5-byte wan_telemetry)  │                ├─▶ RIST pipeline   │
           │  HTTP/JSON control (Bearer token)    │                └─▶ recording       │
           └── POST /start · /stop · GET /status · GET /stats ─────▲                   │
                                                  └────────────────┴───────────────────┘
```

- **Media** stays on RIST/UDP exactly as the encoder produces it; bonded links
  present as multiple RIST peers on the single session port.
- **Control** is REST/JSON over HTTP (cpp-httplib + nlohmann/json), Bearer-token
  authenticated. See `docs/CONTRACT.md`.
- **Telemetry** flows back to the encoder over the RIST OOB channel as the
  encoder's existing 5-byte `wan_telemetry` struct (ABR only, frozen), mirrored
  in `GET /status`; rich per-peer monitoring stats are on `GET /stats`.

## Build

Requirements (verified versions): CMake ≥ 3.28, a C++20 compiler,
GStreamer ≥ 1.28, `nlohmann-json` (system package). `cpp-httplib` is vendored
(`external/httplib.h`). `rist-cpp` (with librist) is vendored under
`external/rist-cpp` and built once via ExternalProject into `build-external/`.

```sh
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release
cmake --build build
```

No encoder/decoder elements are needed — ever. The receiver validates the
required mux/sink elements at `POST /start` and returns
`400 element_unavailable` rather than launching a broken pipeline.

Fast unit-test lane (no GStreamer/librist needed):

```sh
cmake -S . -B build-tests -D OBR_TESTS_ONLY=ON
cmake --build build-tests && ctest --test-dir build-tests
```

## Run (self-hosting quickstart)

The control API binds **127.0.0.1 by default**. Two sane setups:

1. **Loopback + reverse proxy** (recommended): keep the default bind, put
   nginx/Caddy with TLS in front, keep `--token` set.
2. **Direct bind + firewall**: `--bind <ip> --token <long-random>` and
   firewall the control port to your management network.

Refusing footguns: an **empty token on a non-loopback bind refuses to start**
unless you pass `--allow-unauthenticated` (which prints a red warning — anyone
who reaches the port could redirect your stream).

```sh
./build/open-broadcast-receiver \
    --control-port 8080 \
    --rist-port 5000 \
    --token "$RECEIVER_TOKEN" \
    --psk "$RIST_PSK_HEX" --psk-aes 256 \
    --record-dir /var/lib/obr/recordings      # optional
```

Restreaming to boxes on your own LAN? Destination validation blocks private
ranges by default (SSRF hygiene) — opt out with `--egress-allow-private`.

It then idles until the encoder (or `curl`) issues `POST /start` (schema 3):

```sh
curl -s -X POST http://127.0.0.1:8080/start \
  -H "Authorization: Bearer $RECEIVER_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{
        "schema_version": 3,
        "session_id": "demo-1",
        "ingest": { "bandwidth": 8000 },
        "source": { "codec": "h264" },
        "outputs": [
          { "id": "yt",  "type": "rtmp", "url": "rtmp://a.rtmp.example/live2",
            "key_or_streamid": "STREAMKEY" },
          { "id": "cli", "type": "srt",  "url": "srt://203.0.113.9:9000",
            "key_or_streamid": "clientA" }
        ]
      }'

curl -s http://127.0.0.1:8080/status -H "Authorization: Bearer $RECEIVER_TOKEN"
curl -s http://127.0.0.1:8080/stats  -H "Authorization: Bearer $RECEIVER_TOKEN"
curl -s -X POST http://127.0.0.1:8080/stop -H "Authorization: Bearer $RECEIVER_TOKEN" -d '{}'
```

Outputs reconnect **forever** with backoff (1 s → ×2 → 30 s cap) — a platform
ingest outage mid-event heals without operator action. A reconnecting output
rejoins live; the gap simply doesn't exist in the platform-side VOD.

## Security

- **Every** route requires `Authorization: Bearer <token>` (constant-time
  compared). Empty-token mode is gated as described above.
- **RIST PSK**: set `--psk <hex>` / `--psk-aes 256` — without it, raw internet
  UDP reaches the TS parsers. The PSK never appears in any URL or log.
- **Egress validation**: output destinations are resolved and vetted
  (loopback, link-local/metadata, RFC1918/ULA, multicast and `--egress-deny`
  CIDRs rejected), then the vetted IP is pinned for the output's lifetime —
  DNS rebinding after `/start` has nothing to attack.
- Destination URLs and stream keys are validated and never echoed back in
  `GET /status`, `GET /stats`, or logs; pipeline strings carry no secrets.

See `DECISIONS.md` §9 and §13–§17 for the full security rationale.

## Licensing

Licensed under the **GNU Affero General Public License v3.0 or later**
(AGPL-3.0-or-later). See [`LICENSE`](LICENSE).

Third-party components retain their own licenses — see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Contributions are accepted under the same license with a DCO sign-off — see
[`CONTRIBUTING.md`](CONTRIBUTING.md).
