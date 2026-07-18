# CONTRACT.md — encoder ⇄ receiver control & telemetry contract

Date: 2026-07-18 (schema_version 2 — transport profile)
Status: FINAL for v2. This is the shared source of truth both repos implement. The receiver implements the **server**
side, the encoder implements the **client** side. Any change is additive and bumps nothing unless `schema_version` is
incremented.

> **v2 role (2026-07-18).** The receiver is a **copy-only restreamer** again: one incoming RIST/TS stream fans out
> to up to 8 RTMP/RTMPS/SRT/RIST outputs simultaneously — per-output remux (`flvmux` for rtmp/rtmps, TS passthrough
> for srt/rist), H.264+AAC payload, **no decode, no re-encode, no GPU** anywhere on the receiver host. The
> 2026-06-05 decode/v4l2loopback/datarhei role is deleted, not deprecated: its fields are **gone, not ignored**
> (design: `TRANSPORT_PROFILE.md` in the product repo; fan-out architecture recorded in `DECISIONS.md`).

There are **two** channels:

1. **Control plane** — HTTP/1.1 REST + JSON (this document, §1–§7). Reliable, acknowledged, request/response.
2. **Telemetry back-channel** — RIST OOB, a fixed 5-byte binary struct (§8). Best-effort, ~1 Hz. **Frozen.**

The media plane (RIST/UDP, MPEG-TS) is described in GSTREAMER.md.

---

## 1. Transport & framing

- Protocol: **HTTP/1.1**. Server: `httplib::Server` (cpp-httplib) in the receiver. Client: `httplib::Client`
  in the encoder.
- Content type: `application/json` for all request and response bodies (except `GET /healthz` response, also JSON).
- Every request and response body includes `"schema_version": 2` (integer). The receiver MUST reject a request whose
  `schema_version` it does not support with **400 `invalid_schema`**. A v1 body is rejected — there is no
  compatibility shim; encoder and receiver ship together.
- Default bind: receiver `--control-port` (default **8080**) on **`127.0.0.1`** (changed 2026-07-18, FIXPLAN M1.1).
  A non-loopback `--bind` with an **empty token refuses to start** unless the explicit `--allow-unauthenticated`
  flag is passed; no-auth mode logs a prominent warning at every startup. Optional TLS via
  `-DCPPHTTPLIB_OPENSSL_SUPPORT`; hosted deployments terminate TLS at the node reverse proxy.

### Server limits (receiver MUST set)
- `set_payload_max_length(256 * 1024)` (256 KiB; configure bodies are well under this).
- `set_read_timeout(5, 0)` and `set_write_timeout(5, 0)`.
- `set_keep_alive_timeout(5)` and a bounded `set_keep_alive_max_count`.

---

## 2. Authentication

- **Every** route (including `GET /status`, `GET /stats` and `GET /healthz`) requires header:
  `Authorization: Bearer <token>`.
- The token is a long random string delivered out-of-band (encoder config / receiver `--token`), never committed.
- The receiver enforces auth in a `set_pre_routing_handler` using a **constant-time** comparison. On failure it
  returns **401** and does not route. An empty token is an explicit no-auth mode, permitted only on a loopback
  bind or behind `--allow-unauthenticated` (§1).
- The token (header) MUST be redacted in all logs and in `GET /status` output. **Secrets never enter URLs**
  (product-wide house rule): PSKs and stream keys travel in dedicated fields only.

---

## 3. Enums (authoritative — must match encoder `source/lib/lib.h`)

JSON uses **string** enums on the wire for human/curl readability. Both repos map these strings to the encoder's
existing C++ `enum class` values, whose **integer order is fixed** and must not be renumbered.

| JSON field | JSON values (string) | C++ enum (`lib.h`) | int order |
|----------------|-----------------------------|-------------------------------|-----------|
| `source.codec` | `"h264"`, `"h265"`, `"av1"` | `enum class codec : uint8_t` | 0,1,2 |
| `outputs[].type` | `"rtmp"`, `"rtmps"`, `"srt"`, `"rist"` | `enum class output_proto : uint8_t` | 0,1,2,3 |

A receiver MUST reject an unknown enum string with **400 `bad_enum`** naming the offending `field`.
There is no `video.encoder` enum — the receiver has no encoders. Audio is never declared: AAC passes through to
FLV; a non-AAC audio ES degrades rtmp outputs to video-only (§4 validation) rather than failing them.

---

## 4. POST /start — configure + start

Idempotent on an identical body (returns 200 describing the running session). A second `/start` with a
**differing** body or a different `session_id` while running returns **409 `already_running`**. The operator CLI
values (`--rist-port` and the recovery flags) are applied over the body **before** the idempotency comparison, so
idempotency is judged on the effective config.

### Request body
```json
{
  "schema_version": 2,
  "session_id": "uuid-or-monotonic-string",
  "ingest": { "bandwidth": 8000, "buffer_min": 1000, "buffer_max": 5000,
              "rtt_min": 40, "rtt_max": 500, "reorder_buffer": 30 },
  "source": { "codec": "h264" },
  "outputs": [
    { "id": "yt",  "type": "rtmp",  "url": "rtmp://a.rtmp.youtube.com/live2", "key_or_streamid": "…" },
    { "id": "fb",  "type": "rtmps", "url": "rtmps://live-api-s.facebook.com:443/rtmp", "key_or_streamid": "…" },
    { "id": "cli", "type": "srt",   "url": "srt://203.0.113.9:9000", "key_or_streamid": "clientA" }
  ]
}
```

### Field schema

Top level:
| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema_version` | int | yes | must equal 2 |
| `session_id` | string | yes | opaque; echoed back; used for idempotency/stop matching |
| `ingest` | object | no | RIST listener config; CLI recovery flags are authoritative over it |
| `source` | object | yes | the codec arriving over RIST (hint; see below) |
| `outputs` | array | yes (**MAY be empty**) | empty ⇒ the receiver terminates the RIST stream and (if `--record-dir`) records; a legal "link test / record only" session |

`outputs[]`:
| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `id` | string | yes | unique within body; echoed in `/status`, `/stats`, logs |
| `type` | enum output_proto | yes | |
| `url` | string | yes | scheme MUST match `type`; validated, **never echoed**; srt/rist URLs require an explicit port |
| `key_or_streamid` | string | no | RTMP stream key / SRT streamid / RIST n/a; **never echoed or logged** |

Bounds: **max 8 outputs** per session (server-enforced; hosted plans clamp lower at the backplane). Duplicate
`id` ⇒ 400 `bad_enum` field `outputs[].id`; unknown `type` ⇒ 400 `bad_enum`; scheme/type mismatch or unparsable
URL ⇒ 400 `bad_url` naming `outputs[i].url`.

`ingest` (each field optional; defaults shown):
| Field | Type | Default | Bounds | Maps to RIST listen URL param |
|-------|------|---------|--------|-------------------------------|
| `rist_listen` | string | `rist://@[::]:5000` | scheme `rist`/`rist6`, MUST start `@` | base URL (host/port); **operator CLI is authoritative in practice** |
| `bandwidth` | int (kbps) | 6000 | 100–100000 | `bandwidth` + sets `recovery_maxbitrate`; also sizes the fan-out ring |
| `buffer_min` | int (ms) | 1000 | 0–30000 | `buffer-min` + `recovery_length_min` |
| `buffer_max` | int (ms) | 5000 | `buffer_min`–60000 | `buffer-max` + `recovery_length_max` |
| `rtt_min` | int (ms) | 40 | 0–10000 | `rtt-min` + `recovery_rtt_min` |
| `rtt_max` | int (ms) | 500 | `rtt_min`–60000 | `rtt-max` + `recovery_rtt_max` |
| `reorder_buffer` | int (ms) | 30 | 0–10000 | `reorder-buffer` |

The receiver always appends **`timing-mode=0` (SOURCE)** to the listen URL and sets `mProfile = RIST_PROFILE_ADVANCED`
via `RISTNetReceiverSettings`. It does **not** append a `profile=` URL parameter — this build of librist rejects an
unknown `profile` URL param and fails the whole listener. If the default `@[::]` dual-stack bind fails (IPv6-less
host), the receiver retries once on `@0.0.0.0`.

> **Timing mode is SOURCE (`timing-mode=0`) end-to-end — encoder, rist2rist, receiver.** Corrected 2026-07-18:
> this document previously mandated `timing-mode=1` (ARRIVAL), which was wrong and never matched the shipped
> code. ARRIVAL interpolates the arrival time of *retransmitted* packets and asserts
> `packet_time < next->packet_time` (`rist-common.c`); the extra retries of the
> encoder → rist2rist → receiver double hop violate that invariant and SIGABRT the receiver. SOURCE orders and
> paces by the monotonic source timestamp librist stamps on each packet, preserved across the relay, and never
> enters that path. Full history and reproduction: `open-broadcast-encoder/docs/RIST_TIMING_FINDINGS.md`.
> Release builds keep the vendored librist compiled with `-DNDEBUG` as defence in depth (a Debug librist
> re-arms the assert; the receiver logs a warning banner at startup when built without `NDEBUG`).

**Bonding topology:** bonded links present as **multiple RIST peers on the single session port** (one peer per
WAN path, e.g. one `miface` output per WAN from rist2rist, all to the same `host:port`). Port fans
(`port + 2*i`) are **not supported**. Peer connects/disconnects mid-session (cellular CGNAT churn) are normal
and never restart anything.

**PSK:** hosted sessions always set `--psk <hex>` / `--psk-aes <128|256>` on the receiver; self-hosters SHOULD.
The PSK travels via the librist settings struct — never inside any URL.

### Validation (receiver, before anything launches)
- `schema_version != 2` ⇒ **400 `invalid_schema`**.
- Any `ingest.*` numeric out of bounds ⇒ **400 `out_of_range`** naming the field; bad `rist_listen` scheme ⇒
  **400 `bad_url`**.
- Unknown `source.codec` / `outputs[].type` ⇒ **400 `bad_enum`** naming the field.
- **Egress validation (SSRF/rebinding resistance):** every output host is resolved and vetted — loopback,
  link-local (incl. the cloud metadata range), RFC1918/ULA/CGNAT, multicast and the configured
  `--egress-deny` CIDRs are rejected with **400 `forbidden_destination`** naming the output **id** (never the
  resolved IP). The vetted resolved IP is **pinned**: the output pipeline connects to it and never re-resolves.
  Self-host opt-out: `--egress-allow-private` permits RFC1918/ULA (LAN restreaming); loopback/link-local/
  multicast stay blocked. (rtmps connects by hostname — TLS certificate validation is the rebinding defence
  there; see DECISIONS.md.)
- Required mux/sink element absent from the GStreamer registry (`flvmux`, `rtmp2sink`, `srtsink`, `ristsink`,
  `tsdemux`, `h264parse`, `aacparse`, `tsparse`) ⇒ **400 `element_unavailable`** naming the element. Checked
  synchronously at `/start`. (`encoder_unavailable` no longer exists — there are no encoders.)
- `source.codec != "h264"` with any rtmp/rtmps output ⇒ **400 `rtmp_codec_unsupported`** (fail early on the
  hint). If runtime detection later finds a non-H.264 video ES while rtmp outputs exist, those outputs enter
  `error` state `rtmp_codec_unsupported` (session survives; srt/rist outputs continue). A non-AAC **audio** ES
  with rtmp outputs ⇒ those outputs run **video-only** and report `audio_dropped: true` rather than failing.

### Response 200 (started or already-running-identical)
```json
{
  "ok": true,
  "schema_version": 2,
  "session_id": "uuid-or-monotonic-string",
  "state": "running",
  "outputs": [ { "id": "yt", "type": "rtmp" }, { "id": "fb", "type": "rtmps" }, { "id": "cli", "type": "srt" } ]
}
```
The echo carries **id and type only** — URLs and keys are never echoed.

### Error responses
- **400** invalid input:
  ```json
  { "ok": false, "schema_version": 2, "error_code": "invalid_schema|bad_url|bad_enum|out_of_range|forbidden_destination|element_unavailable|rtmp_codec_unsupported", "message": "human readable", "field": "outputs[1].url" }
  ```
- **401** unauthenticated: `{ "ok": false, "error_code": "unauthorized" }` (no schema parsing required).
- **409** already running:
  ```json
  { "ok": false, "schema_version": 2, "error_code": "already_running", "session_id": "<current session_id>" }
  ```
- **500** pipeline launch failure:
  ```json
  { "ok": false, "schema_version": 2, "error_code": "pipeline_launch_failed", "message": "<error>" }
  ```

---

## 5. POST /stop

### Request body
```json
{ "schema_version": 2, "session_id": "uuid-or-monotonic-string" }
```
Both fields are optional on `/stop` (it is intentionally lenient so a stuck encoder can always halt the stream): an
absent or malformed body is treated as an **unconditional stop**, and `schema_version` is **not** enforced here (unlike
`/start`). If `session_id` is present and does not match the running session ⇒ **409 `session_mismatch`**
`{ "ok": false, "error_code": "session_mismatch", "session_id": "<current>" }`. Stopping when already stopped is a
no-op returning 200.

### Response 200
```json
{ "ok": true, "schema_version": 2, "state": "stopped" }
```

---

## 6. GET /status

Poll for health/state, decoupled from RIST. Recommended encoder poll interval **1–2 s**.

### Response 200
```json
{
  "ok": true, "schema_version": 2, "state": "running", "session_id": "s_9f2c", "uptime_s": 1234,
  "telemetry": { "link_quality": 92, "worst_case_rtt_ms": 140 },
  "outputs": [
    { "id": "yt",  "type": "rtmp",  "state": "running", "connected_s": 1230, "reconnects": 0, "audio_dropped": false },
    { "id": "fb",  "type": "rtmps", "state": "reconnecting", "reconnects": 3, "audio_dropped": false, "last_error": "connection_refused" },
    { "id": "cli", "type": "srt",   "state": "running", "connected_s": 1231, "reconnects": 0, "audio_dropped": false }
  ],
  "recording": { "active": true, "bytes": 912345678 },
  "last_bus_error": null
}
```
- Session `state` ∈ `"running"`, `"stopped"`, `"error"`. It stays `running` while **any** output runs or the
  session is intact with zero outputs; **per-output failure is never session-fatal**. `error` is reserved for
  session-level failures (`idle_timeout`, listener death), reported with `last_bus_error` until the next `/start`.
- Per-output `state` ∈ `starting | running | reconnecting | error | stopped`. **Reconnect policy: every output
  failure retries forever** (backoff 1 s → ×2 → 30 s cap; a run stable for 30 s resets the ladder). The only
  terminal per-output state (`error`) is `rtmp_codec_unsupported` — auth-shaped failures are indistinguishable
  from transient resets at the sink and deliberately retry; "check your stream key" advisories belong in the
  panel/agent on top of `reconnects`/`last_error`.
- A reconnecting output **rejoins live**: the gap is absent from the platform-side VOD; backlog is never replayed.
- `telemetry` **mirrors the 5-byte RIST-OOB `wan_telemetry`** (§8). When stopped, `telemetry` MAY be absent.
- When stopped: `{ "ok": true, "schema_version": 2, "state": "stopped", "session_id": null, "outputs": [] }`.

---

## 6a. GET /stats (agent-facing)

Polled by the node agent at ~1 Hz (same bearer token — deliberate: agent and encoder share the per-session trust
domain). The handler reads atomics/snapshots only; it MUST NOT take pipeline locks or block the distribution path.

### Response 200 (shape)
```json
{
  "ok": true, "schema_version": 2, "session_id": "s_9f2c", "ts": 1789620000123,
  "ring_size_bytes": 16777216,
  "rist": {
    "quality": 97.4, "rtt_ms": 41, "received": 182034, "missing": 210, "recovered": 208,
    "recovered_one_retry": 190, "lost": 2, "reordered": 44,
    "bandwidth_bps": 6210000, "retry_bandwidth_bps": 12000,
    "peers": [
      { "id": 1, "rtt_ms": 38, "avg_rtt_ms": 40.1, "received": 91000, "received_bytes": 119000000, "bandwidth_bps": 3100000 },
      { "id": 2, "rtt_ms": 122, "avg_rtt_ms": 118.0, "received": 91034, "received_bytes": 119100000, "bandwidth_bps": 3110000 }
    ]
  },
  "ts_in": { "bytes_total": 238100000 },
  "outputs": [ { "id": "yt", "state": "running", "bytes_sent": 912345678, "dropped_bytes": 0, "reconnects": 0 } ],
  "recording": { "active": true, "bytes": 912345678, "dropped_bytes": 0 }
}
```
- `rist.peers[]`: one entry per connected RIST peer — **each link in a bond arrives as a separate peer**, so this
  is the per-WAN-path view. Per-peer counters come from the vendored librist's `rist_stats_receiver_peer`
  (verified present).
- `ring_size_bytes`: the fan-out ring chosen at `/start` — `clamp(10 s × ingest.bandwidth, 16 MiB, 256 MiB)`.
- `outputs[].dropped_bytes`: ring drop-oldest accounting for that consumer (slow-output back-pressure never
  reaches ingest). `recording` appears when a recorder exists.
- `ts_in.bytes_total`: total ingest payload bytes (the agent derives bitrate from deltas).

---

## 7. GET /healthz

Liveness only, still token-gated.
### Response 200
```json
{ "ok": true }
```
Returns 200 whenever the process and HTTP server are alive, regardless of pipeline state.

---

## 8. RIST OOB telemetry (out-of-band binary back-channel)

**Unchanged, byte-for-byte, and frozen.** This is **not** HTTP. It travels over the RIST control channel
(available only in RIST_PROFILE_MAIN/ADVANCED; both ends are ADVANCED here). The receiver sends, the encoder
receives, on a ~1 Hz cadence aligned to the receiver's `statisticsCallback`. It is an **ABR control signal from
the bottleneck hop**, never a monitoring feed — monitoring uses `GET /stats` (§6a) on a separate channel.

### Exact wire layout (5 bytes, byte-for-byte identical to encoder `lib.h:53-59`)
```c
struct __attribute__((packed)) wan_telemetry {
  uint8_t  link_quality;     // 0..100, raw byte (no byte order)
  uint32_t worst_case_rtt;   // milliseconds, NETWORK byte order on the wire
};
static_assert(sizeof(wan_telemetry) == 5, "wan_telemetry must be exactly 5 bytes");
```

| Offset | Size | Field | Encoding |
|--------|------|-------|----------|
| 0 | 1 | `link_quality` | unsigned byte, 0–100 |
| 1 | 4 | `worst_case_rtt` | uint32, big-endian (`htonl` on send, `ntohl` on receive) |

Total: **exactly 5 bytes.** The encoder validates `size != sizeof(wan_telemetry)` and silently drops anything not
exactly 5. The receiver MUST send exactly 5 bytes.

### Value derivation (receiver)
- Guard `stats.stats_type == RIST_STATS_RECEIVER_FLOW`, read `stats.stats.receiver_flow`.
- `link_quality = round(clamp(receiver_flow.quality, 0, 100))` where `quality = received*100/(received+missing)`.
- `worst_case_rtt_ms = max(receiver_flow.rtt, max(receiver_flow.peers[i].rtt))` — the worst path in the bond.
- Send to the most recent data peer (captured from `networkDataCallback`; cleared on disconnect — a surviving
  bonded peer re-captures it with its next payload).

### Reliability
OOB is best-effort (no retransmit/ordering). Occasional loss is acceptable for 1 Hz telemetry. The receiver MUST NOT
let an OOB send failure tear down media. `GET /status` `telemetry` is the reliable mirror of these same two values.
