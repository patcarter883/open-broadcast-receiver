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

Container image (tagged with the commit's short SHA plus `:latest`):

```sh
make image          # see docs/IMAGE.md
```

No encoder/decoder elements are needed — ever. The receiver validates the
required mux/sink elements at `POST /start` and returns
`400 element_unavailable` rather than launching a broken pipeline.

Fast unit-test lane (no GStreamer/librist needed):

```sh
cmake -S . -B build-tests -D OBR_TESTS_ONLY=ON
cmake --build build-tests && ctest --test-dir build-tests
```

RTMP output path, end to end, against a local sink (no rig, no platform):

```sh
test/harness/rtmp_publish_harness.sh    # see test/harness/README.md
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

### Trunk ingest (dial a relay instead of listening)

By default the receiver **binds** `--rist-port` and waits for the sender to dial
in. A BYON venue usually cannot dial a hosted node at all (the venue and the node
are each behind NAT), so the venue's media crosses a relay and the node's
receiver **dials the relay's listening sender**:

```sh
./build/open-broadcast-receiver \
    --connect-url rist://relay.example.net:6010 \
    --srp-user "$TRUNK_SRP_USER" --srp-pass "$TRUNK_SRP_PASS" \
    --token "$RECEIVER_TOKEN"
```

`--connect-url` replaces the listen socket: the process opens **no** RIST port
and combining it with an explicit `--rist-port` or with a non-default listen URL
is refused rather than half-applied. The control plane is unchanged.
`--srp-user`/`--srp-pass` are the trunk's SRP client credentials, sent in
librist's own URL parameters (`username=`/`password=`, percent-encoded); the URL
is never logged past its `?`.

### Relay mode (the trunk bridge itself)

The same binary can run as the relay: receive on one bound RIST leg, send on
another, forwarding every packet unchanged (no remux, no restamp, sequence
numbers preserved). It takes **no** control port, no token and no pipeline.

```sh
./build/open-broadcast-receiver \
    --relay-in  'rist://@[::]:5000' \
    --relay-out 'rist://@[::]:6010' \
    --relay-srp /run/obr/trunk-srp.txt        # ristsrppasswd file for the out leg
    # optional: --relay-secret "$TRUNK_AES_KEY" --relay-aes 256
```

Both legs bind (`@` required). The out leg authenticates the node's receiver
with the SRP file; the AES secret rides the peer config, never the URL. A relay
that cannot bind either leg exits non-zero instead of pumping into nothing.
Counters are printed once a second as `relay stats: in_packets=… out_packets=…`.
`test/harness/relay_trunk_e2e.sh` runs the whole chain and judges it by
sequence continuity after the handshake.

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
