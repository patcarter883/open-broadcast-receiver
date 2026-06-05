# CONTRACT.md — encoder ⇄ receiver control & telemetry contract

Date: 2026-06-05
Status: FINAL for v1. This is the shared source of truth both repos implement. The receiver implements the **server**
side, the encoder implements the **client** side. Any change is additive and bumps nothing unless `schema_version` is
incremented.

> **v1 role change (2026-06-05).** The receiver no longer encodes/muxes to RTMP/SRT/RIST. It now **decodes** the
> incoming stream and hands the **uncompressed** result to a local restreaming package (datarhei/restreamer): raw video
> to a v4l2loopback device, PCM audio to an ALSA snd-aloop device. That package owns all further encoding +
> restreaming. Consequently `POST /start` no longer carries an `outputs[]` array (§4). The handoff devices are operator
> infrastructure set via the receiver's CLI (`--v4l2-device` / `--audio-device` / `--pixel-format` / `--no-hw-decode`),
> not over the control plane. For transitional compatibility the server **ignores** any unknown body fields (so an
> as-yet-unmodified encoder that still sends `outputs[]` continues to work — the array is simply dropped).

There are **two** channels:

1. **Control plane** — HTTP/1.1 REST + JSON (this document, §1–§7). Reliable, acknowledged, request/response.
2. **Telemetry back-channel** — RIST OOB, a fixed 5-byte binary struct (§8). Best-effort, ~1 Hz.

The media plane (RIST/UDP, MPEG-TS) is described in GSTREAMER.md and is unchanged from the existing encoder.

---

## 1. Transport & framing

- Protocol: **HTTP/1.1**. Server: `httplib::Server` (cpp-httplib 0.40.0) in the receiver. Client: `httplib::Client`
  in the encoder.
- Content type: `application/json` for all request and response bodies (except `GET /healthz` response, also JSON).
- Every request and response body includes `"schema_version": 1` (integer). The receiver MUST reject a request whose
  `schema_version` it does not support with **400 `invalid_schema`**.
- Default bind: receiver `--control-port` (default **8080**) on a private/management interface; `0.0.0.0` only behind a
  source-IP allowlist / WireGuard. Optional TLS via `-DCPPHTTPLIB_OPENSSL_SUPPORT`.

### Server limits (receiver MUST set)
- `set_payload_max_length(256 * 1024)` (256 KiB; configure bodies are well under this).
- `set_read_timeout(5, 0)` and `set_write_timeout(5, 0)`.
- `set_keep_alive_timeout(5)` and a bounded `set_keep_alive_max_count`.

---

## 2. Authentication

- **Every** route (including `GET /status` and `GET /healthz`) requires header:
  `Authorization: Bearer <token>`.
- The token is a long random string delivered out-of-band (encoder config / receiver `--token`), never committed.
- The receiver enforces auth in a `set_pre_routing_handler` using a **constant-time** comparison. On failure it
  returns **401** and does not route. If the receiver is started with an empty token (`--token` omitted), it runs in
  an explicit dev/no-auth mode (documented; not for production).
- The token (header) MUST be redacted in all logs and in `GET /status` output.

---

## 3. Enums (authoritative — must match encoder `source/lib/lib.h`)

JSON uses **string** enums on the wire for human/curl readability. Both repos map these strings to the encoder's
existing C++ `enum class` values, whose **integer order is fixed** and must not be renumbered.

| JSON field     | JSON values (string)        | C++ enum (`lib.h`)            | int order |
|----------------|-----------------------------|-------------------------------|-----------|
| `source.codec` | `"h264"`, `"h265"`, `"av1"` | `enum class codec : uint8_t`  | 0,1,2     |

The audio codec is never declared — it is always **detected** from the incoming MPEG-TS (AAC/Opus/AC-3/E-AC-3/MPEG-1-2
audio) and decoded to PCM for the handoff. A receiver MUST reject an unknown `source.codec` string with
**400 `bad_enum`** naming the offending `field`.

> Removed in this v1 role change: `video.encoder` (`amd|qsv|nvenc|software`) and `outputs[].type`
> (`rtmp|rtmps|srt|rist`). The receiver picks the decoder itself (NVDEC/VA/QSV/software, see GSTREAMER.md §4) and does
> not encode or open network outputs.

---

## 4. POST /start — configure + start

Idempotent on an identical *effective* body (returns 200 describing the running session). A second concurrent `/start`
with a **differing** body or a different `session_id` while running returns **409 `already_running`**. The operator
CLI values (`--rist-port`, the recovery flags, and the raw-sink device flags) are applied over the body **before** the
idempotency comparison, so idempotency is judged on the effective config.

### Request body
```json
{
  "schema_version": 1,
  "session_id": "uuid-or-monotonic-string",
  "ingest": {
    "rist_listen": "rist://@[::]:5000",
    "bandwidth": 6000,
    "buffer_min": 1000,
    "buffer_max": 5000,
    "rtt_min": 40,
    "rtt_max": 500,
    "reorder_buffer": 30
  },
  "source": { "codec": "h264" }
}
```

### Field schema, types, defaults, bounds

Top level:
| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema_version` | int | yes | must equal 1 |
| `session_id` | string | yes | opaque; echoed back; used for idempotency/stop matching |
| `ingest` | object | no | RIST listener config; CLI recovery flags are authoritative over it |
| `source` | object | yes | the codec arriving over RIST |

`ingest` (each field optional; defaults shown):
| Field | Type | Default | Bounds | Maps to RIST listen URL param |
|-------|------|---------|--------|-------------------------------|
| `rist_listen` | string | `rist://@[::]:5000` | scheme `rist`/`rist6`, MUST start `@` | base URL (host/port) |
| `bandwidth` | int (kbps) | 6000 | 100–100000 | `bandwidth` + sets `recovery_maxbitrate` |
| `buffer_min` | int (ms) | 1000 | 0–30000 | `buffer-min` + `recovery_length_min` |
| `buffer_max` | int (ms) | 5000 | `buffer_min`–60000 | `buffer-max` + `recovery_length_max` |
| `rtt_min` | int (ms) | 40 | 0–10000 | `rtt-min` + `recovery_rtt_min` |
| `rtt_max` | int (ms) | 500 | `rtt_min`–60000 | `rtt-max` + `recovery_rtt_max` |
| `reorder_buffer` | int (ms) | 30 | 0–10000 | `reorder-buffer` |

The receiver always appends `timing-mode=1` (ARRIVAL) to the listen URL and sets `mProfile = RIST_PROFILE_ADVANCED`
via `RISTNetReceiverSettings`. It does **not** append a `profile=` URL parameter — this build of librist rejects an
unknown `profile` URL param and fails the whole listener.

`source`:
| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `codec` | enum codec | `"h264"` | a **hint** of the elementary stream. The receiver detects the actual video **and** audio codec from the incoming MPEG-TS (tsdemux pad caps) and uses that to select the decoder; `source.codec` is only the fallback if detection times out. |

**Raw-sink handoff (NOT in the body — operator CLI / `raw_sink_config`).** The decoded output targets are fixed host
infrastructure, set when the receiver process starts:

| CLI flag | Default | Meaning |
|----------|---------|---------|
| `--v4l2-device` | `/dev/video10` | v4l2loopback device the raw video frames are written to (`v4l2sink`) |
| `--audio-device` | `hw:Loopback,0,0` | ALSA snd-aloop device the PCM audio is written to (`alsasink`) |
| `--pixel-format` | `NV12` | raw video pixel format presented on the device |
| `--no-hw-decode` | (hw preferred) | force software decode instead of NVDEC/VA/QSV |

### Validation (receiver, before `gst_parse_launch`)
- `schema_version != 1` ⇒ **400 `invalid_schema`**.
- Any `ingest.*` numeric out of bounds ⇒ **400 `out_of_range`** naming the field; bad `rist_listen` scheme ⇒
  **400 `bad_url`**.
- Unknown `source.codec` ⇒ **400 `bad_enum`**, field `source.codec`.
- The CLI-supplied sink device / pixel-format values are checked for shell/caps-meta safety
  (`bad_url` / `bad_enum`) — they cannot contain quote-escape or control characters.
- Requested decoder/sink element not present in the GStreamer registry on this box (validated via registry lookup at
  start — output-side `v4l2sink`/`alsasink`/converters synchronously, the codec-specific decoder once detection runs)
  ⇒ **400 `encoder_unavailable`**, naming the missing element.

### Response 200 (started or already-running-identical)
```json
{
  "ok": true,
  "schema_version": 1,
  "session_id": "uuid-or-monotonic-string",
  "state": "running",
  "sink": { "video_device": "/dev/video10", "audio_device": "hw:Loopback,0,0", "state": "connecting" }
}
```

### Error responses
- **400** invalid input:
  ```json
  { "ok": false, "schema_version": 1, "error_code": "invalid_schema|bad_url|bad_enum|out_of_range|encoder_unavailable", "message": "human readable", "field": "ingest.bandwidth" }
  ```
- **401** unauthenticated: `{ "ok": false, "error_code": "unauthorized" }` (no schema parsing required).
- **409** already running:
  ```json
  { "ok": false, "schema_version": 1, "error_code": "already_running", "session_id": "<current session_id>" }
  ```
- **500** pipeline launch failure:
  ```json
  { "ok": false, "schema_version": 1, "error_code": "pipeline_launch_failed", "message": "<gst error>" }
  ```

---

## 5. POST /stop

### Request body
```json
{ "schema_version": 1, "session_id": "uuid-or-monotonic-string" }
```
Both fields are optional on `/stop` (it is intentionally lenient so a stuck encoder can always halt the stream): an
absent or malformed body is treated as an **unconditional stop**, and `schema_version` is **not** enforced here (unlike
`/start`). If `session_id` is present and does not match the running session ⇒ **409 `session_mismatch`**
`{ "ok": false, "error_code": "session_mismatch", "session_id": "<current>" }`. Stopping when already stopped is a
no-op returning 200.

### Response 200
```json
{ "ok": true, "schema_version": 1, "state": "stopped" }
```

---

## 6. GET /status

Poll for health/state, decoupled from RIST. Recommended encoder poll interval **1–2 s**.

### Response 200
```json
{
  "ok": true,
  "schema_version": 1,
  "state": "running",
  "session_id": "uuid-or-monotonic-string",
  "uptime_s": 1234,
  "telemetry": { "link_quality": 92, "worst_case_rtt_ms": 140 },
  "sink": {
    "video_device": "/dev/video10",
    "audio_device": "hw:Loopback,0,0",
    "video_codec": "h264",
    "audio_codec": "aac",
    "state": "running"
  },
  "last_bus_error": null
}
```
- `state` ∈ `"running"`, `"stopped"`, `"error"`.
- `telemetry` **mirrors the 5-byte RIST-OOB `wan_telemetry`** (§8): `link_quality` 0–100, `worst_case_rtt_ms`
  uint32 milliseconds. When stopped, `telemetry` MAY be `null`.
- `sink.video_codec` / `sink.audio_codec` are the codecs **detected** off the live MPEG-TS; they are `null` until
  phase-1 detection settles. `sink.state` ∈ `"connecting"`, `"running"`, `"error"`.
- When stopped: `{ "ok": true, "schema_version": 1, "state": "stopped", "session_id": null, "sink": null }`.

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

This is **not** HTTP. It travels over the RIST control/GRE channel (available only in RIST_PROFILE_MAIN/ADVANCED; both
ends are ADVANCED here). The receiver sends, the encoder receives, on a ~1 Hz cadence aligned to the receiver's
`statisticsCallback`.

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
exactly 5 (`main.cpp:79`). The receiver MUST send exactly 5 bytes.

### Value derivation (receiver)
- Guard `stats.stats_type == RIST_STATS_RECEIVER_FLOW`, read `stats.stats.receiver_flow`.
- `link_quality = round(clamp(receiver_flow.quality, 0, 100))` where `quality = received*100/(received+missing)`.
- `worst_case_rtt_ms = receiver_flow.rtt`, optionally `max(receiver_flow.peers[i].rtt)` for true worst-case.
- Send to the single connected encoder peer (captured from `networkDataCallback`/`getActiveClients`; cleared in
  `clientDisconnectedCallback`).

### Reliability
OOB is best-effort (no retransmit/ordering). Occasional loss is acceptable for 1 Hz telemetry. The receiver MUST NOT
let an OOB send failure tear down media. `GET /status` `telemetry` is the reliable mirror of these same two values.
