# CONTRACT.md — encoder ⇄ receiver control & telemetry contract

Date: 2026-05-30
Status: FINAL for v1. This is the shared source of truth both repos implement. The receiver implements the **server**
side, the encoder implements the **client** side. Any change is additive and bumps nothing unless `schema_version` is
incremented.

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
- The token (header) and every `key_or_streamid` (body) MUST be redacted in all logs and in `GET /status` output.

---

## 3. Enums (authoritative — must match encoder `source/lib/lib.h`)

JSON uses **string** enums on the wire for human/curl readability. Both repos map these strings to the encoder's
existing C++ `enum class` values, whose **integer order is fixed** and must not be renumbered.

| JSON field           | JSON values (string)                | C++ enum (`lib.h`)                          | int order |
|----------------------|-------------------------------------|---------------------------------------------|-----------|
| `*.codec`            | `"h264"`, `"h265"`, `"av1"`         | `enum class codec : uint8_t`                | 0,1,2     |
| `video.encoder`      | `"amd"`, `"qsv"`, `"nvenc"`, `"software"` | `enum class encoder : uint8_t`        | 0,1,2,3   |
| `outputs[].type`     | `"rtmp"`, `"rtmps"`, `"srt"`, `"rist"` | `enum class output_proto` (receiver)     | see note  |
| `*.video.mode`       | `"copy"`, `"reencode"`              | bool `reencode` (`reencode == (mode=="reencode")`) | —  |
| `*.audio.mode`       | `"copy"`, `"reencode"`              | bool `reencode`                             | —         |
| `audio.codec`        | `"aac"` (only)                      | —                                           | —         |

Note on `output_proto`: the encoder's new `enum class output_proto { rtmp, srt, rist, udp }` (added to lib.h, §IMPL).
On the wire, `"rtmps"` maps to the rtmp branch with a `rtmps://` URL (TLS RTMP). `"udp"` is reserved (not surfaced in
v1 UI). The receiver MUST reject an unknown `type` string with 400 `bad_enum`.

A receiver MUST reject any unknown enum string with **400 `bad_enum`** naming the offending `field`.

---

## 4. POST /start — configure + start

Idempotent on an identical body (returns 200 describing the running session). A second concurrent `/start` with a
**differing** body or a different `session_id` while running returns **409 `already_running`**.

### Request body
```json
{
  "schema_version": 1,
  "session_id": "uuid-or-monotonic-string",
  "ingest": {
    "rist_listen": "rist://@[::]:5000",
    "bandwidth": 6000,
    "buffer_min": 245,
    "buffer_max": 5000,
    "rtt_min": 40,
    "rtt_max": 500,
    "reorder_buffer": 240
  },
  "source": { "codec": "h264" },
  "outputs": [
    {
      "id": "primary",
      "type": "rtmp",
      "url": "rtmp://host/app",
      "key_or_streamid": "secret",
      "params": { "latency_ms": 200 },
      "video": {
        "mode": "reencode",
        "codec": "h264",
        "encoder": "software",
        "bitrate_kbps": 4300,
        "upscale": false,
        "width": 2560,
        "height": 1440
      },
      "audio": { "mode": "copy", "codec": "aac", "bitrate_kbps": 128 }
    }
  ]
}
```

### Field schema, types, defaults, bounds

Top level:
| Field | Type | Required | Default | Notes |
|-------|------|----------|---------|-------|
| `schema_version` | int | yes | — | must equal 1 |
| `session_id` | string | yes | — | opaque; echoed back; used for idempotency/stop matching |
| `ingest` | object | yes | — | RIST listener config |
| `source` | object | yes | — | the codec arriving over RIST |
| `outputs` | array(object) | yes | — | ≥1 element; each is one destination |

`ingest`:
| Field | Type | Default | Bounds | Maps to RIST listen URL param |
|-------|------|---------|--------|-------------------------------|
| `rist_listen` | string | `rist://@[::]:5000` | scheme `rist`/`rist6`, MUST start `@` | base URL (host/port) |
| `bandwidth` | int (kbps) | 6000 | 100–100000 | `bandwidth` + sets `recovery_maxbitrate` |
| `buffer_min` | int (ms) | 245 | 0–30000 | `buffer-min` + `recovery_length_min` |
| `buffer_max` | int (ms) | 5000 | `buffer_min`–60000 | `buffer-max` + `recovery_length_max` |
| `rtt_min` | int (ms) | 40 | 0–10000 | `rtt-min` + `recovery_rtt_min` |
| `rtt_max` | int (ms) | 500 | `rtt_min`–60000 | `rtt-max` + `recovery_rtt_max` |
| `reorder_buffer` | int | 240 | 0–10000 | `reorder-buffer` (URL only — wrapper omits struct copy) |

The receiver always appends `timing-mode=2` to the listen URL and sets `mProfile = RIST_PROFILE_ADVANCED` via
`RISTNetReceiverSettings`. It does **not** append a `profile=` URL parameter — this build of librist rejects an
unknown `profile` URL param and fails the whole listener (`Unknown or invalid parameter profile`). These values mirror
the encoder's `output_config` defaults (lib.h:94-106).

`source`:
| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `codec` | enum codec | `"h264"` | a **hint** of the elementary stream the encoder is sending. The receiver detects the actual video **and** audio codec from the incoming MPEG-TS (tsdemux pad caps) and uses that to select the parser/decoder; `source.codec` is only the fallback if detection times out. The audio codec is never declared — it is always detected (AAC/Opus/AC-3/E-AC-3/MPEG-1-2 audio), and any non-AAC input is transcoded to AAC. |

`outputs[]` (one per destination):
| Field | Type | Required | Default | Notes |
|-------|------|----------|---------|-------|
| `id` | string | yes | — | unique per output; used in status and to name pipeline elements |
| `type` | enum output_proto | yes | — | `rtmp`/`rtmps`/`srt`/`rist` |
| `url` | string | yes | — | scheme MUST be in the allowlist matching `type`; no shell-meta/quote chars |
| `key_or_streamid` | string | no | `""` | RTMP stream key, or SRT streamid; redacted in logs/status |
| `params` | object | no | `{}` | per-type tuning (below) |
| `video` | object | yes | — | disposition + codec/encoder |
| `audio` | object | no | `{mode:"copy",codec:"aac"}` | |

`outputs[].params` (all optional):
| Field | Type | Applies to | Default | Bounds |
|-------|------|-----------|---------|--------|
| `latency_ms` | int | srt | 200 | 0–8000 |
| `sender_buffer` | int | rist | = `ingest.bandwidth`-derived | 0–10000 |
| `cname` | string | rist | `""` | |

`outputs[].video`:
| Field | Type | Required | Default | Bounds | Notes |
|-------|------|----------|---------|--------|-------|
| `mode` | `"copy"`/`"reencode"` | yes | — | — | copy = passthrough parse only |
| `codec` | enum codec | yes | — | — | in copy mode MUST equal `source.codec` (else 400 `bad_enum`) |
| `encoder` | enum encoder | reencode only | `"software"` | — | ignored in copy mode |
| `bitrate_kbps` | int | reencode only | 4300 | 1000–60000 | maps to `videncoder bitrate={}` |
| `upscale` | bool | no | false | — | reencode only; ignored in copy mode |
| `width` | int | upscale only | 2560 | 16–7680 | even |
| `height` | int | upscale only | 1440 | 16–4320 | even |

`outputs[].audio`:
| Field | Type | Required | Default | Bounds | Notes |
|-------|------|----------|---------|--------|-------|
| `mode` | `"copy"`/`"reencode"` | no | `"copy"` | — | |
| `codec` | `"aac"` | no | `"aac"` | — | only aac supported |
| `bitrate_kbps` | int | reencode only | 128 | 32–512 | maps to avenc_aac `bitrate={}*1000` |

### Cross-field validation (receiver, before `gst_parse_launch`)
- `type` ∈ {rtmp,rtmps} **and** effective video codec ∈ {h265,av1} ⇒ **400 `rtmp_codec_unsupported`**, field
  `outputs[i].video.codec` (RTMP/flvmux carries H.264+AAC only — see GSTREAMER.md).
- `video.mode == "copy"` **and** `video.codec != source.codec` ⇒ **400 `bad_enum`**, field `outputs[i].video.codec`.
- Injection defence (as implemented in `is_pipeline_safe` + `validate_config`): every value interpolated into the
  `gst_parse_launch` string is wrapped in single quotes, so the validator bans only the characters that could break
  out of a single-quoted property — `'`, `"`, `\`, and ASCII control chars (NOT `&`/`?`/`=`, which appear in legitimate
  SRT/RIST query URLs). Any of the banned characters in `url`/`key_or_streamid`/`cname` ⇒ **400 `bad_url`**.
  Additionally, for `srt`/`rist` the `url` authority must parse to a `host:port` and the **host** must be a clean token
  (`[A-Za-z0-9.:_\-\[\]]` only) since the RIST `address` is a bare property; otherwise **400 `bad_url`**.
- `type == rist` **and** the port is **odd** ⇒ **400 `out_of_range`**, field `outputs[i].url` (RIST RTCP uses
  `port+1`, so the data port must be even).
- `url` scheme not in the allowlist for `type` ⇒ **400 `bad_url`**, field `outputs[i].url`.
- Requested `encoder`/`codec` family element not present in the GStreamer registry on this box (validated via
  `gst-inspect`/registry lookup at start) ⇒ **400 `encoder_unavailable`**, field `outputs[i].video.encoder`.
- Any numeric out of bounds ⇒ **400 `out_of_range`** naming the field.
- Missing required field or wrong JSON type ⇒ **400 `invalid_schema`** naming the field.

### Response 200 (started or already-running-identical)
```json
{
  "ok": true,
  "schema_version": 1,
  "session_id": "uuid-or-monotonic-string",
  "state": "running",
  "outputs": [ { "id": "primary", "state": "connecting" } ]
}
```
`outputs[].state` ∈ `"connecting"`, `"connected"`, `"error"`.

### Error responses
- **400** invalid input:
  ```json
  { "ok": false, "schema_version": 1, "error_code": "invalid_schema|bad_url|bad_enum|out_of_range|rtmp_codec_unsupported|encoder_unavailable", "message": "human readable", "field": "outputs[0].url" }
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
  "outputs": [
    { "id": "primary", "type": "rtmp", "state": "connected", "bitrate_kbps": 4280, "last_error": null }
  ],
  "last_bus_error": null
}
```
- `state` ∈ `"running"`, `"stopped"`, `"error"`.
- `telemetry` **mirrors the 5-byte RIST-OOB `wan_telemetry`** (§8): `link_quality` 0–100, `worst_case_rtt_ms`
  uint32 milliseconds. When stopped, `telemetry` MAY be `null`.
- `outputs[].state` ∈ `"connecting"`, `"connected"`, `"error"`. `last_error`/`last_bus_error` are redacted strings
  or `null`.
- When stopped: `{ "ok": true, "schema_version": 1, "state": "stopped", "session_id": null, "outputs": [] }`.

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

### Sender (receiver side) — build explicitly
```c
uint8_t pkt[5];
pkt[0] = link_quality_0_100;        // from receiver_flow.quality, clamped/rounded
uint32_t be = htonl(worst_rtt_ms);  // from receiver_flow.rtt (or max over peers)
memcpy(&pkt[1], &be, 4);
receiver.sendOOBData(peer, pkt, sizeof(pkt));  // exactly 5 bytes
```

### Value derivation (receiver)
- Guard `stats.stats_type == RIST_STATS_RECEIVER_FLOW`, read `stats.stats.receiver_flow`.
- `link_quality = round(clamp(receiver_flow.quality, 0, 100))` where `quality = received*100/(received+missing)`.
- `worst_case_rtt_ms = receiver_flow.rtt`, optionally `max(receiver_flow.peers[i].rtt)` for true worst-case.
- Send to the single connected encoder peer (captured from `networkDataCallback`/`getActiveClients`; cleared in
  `clientDisconnectedCallback`).

### Encoder side (already implemented — do not change)
`rist_oob_cb` (`main.cpp:77`): gate size==5, `memcpy`, `ntohl(worst_case_rtt)`, store into `stats.wan_quality` /
`stats.wan_rtt`, update UI, and — when `scaling_source == remote_oob` — drive `scale_encoder_bitrate`.

### Reliability
OOB is best-effort (no retransmit/ordering). Occasional loss is acceptable for 1 Hz telemetry. The receiver MUST NOT
let an OOB send failure tear down media — the vendored wrapper's `sendOOBData` is patched to log+return false instead
of `destroyReceiver()` (see DECISIONS §6). Gate the first OOB send on an established connection. `GET /status`
`telemetry` is the reliable mirror of these same two values.
