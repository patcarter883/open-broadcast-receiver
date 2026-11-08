# CONTRACT.md — encoder ⇄ receiver control & telemetry contract

Date: 2026-11-08 (schema_version 5 — standby/slate source + source geometry)
Status: FINAL for v5. This is the shared source of truth both repos implement. The receiver implements the **server**
side, the encoder implements the **client** side. Any change is additive and bumps nothing unless `schema_version` is
incremented.

> **v5 additions (2026-11-08).** A `standby` object at the top level of `POST /start` (a slate/source the receiver
> can hold on air when live media is absent; **disabled unless asked for**) and `source.width`/`height`/`fps`
> (the arriving picture's geometry; `0` = unknown). Both are optional and additive: a v3/v4 body's meaning is
> unchanged. **v4 is still accepted, transitionally** — see §1.

> **v3 role (2026-10-03).** The receiver is still a **copy-only restreamer by default**: one incoming RIST/TS stream
> fans out to up to 8 RTMP/RTMPS/SRT/RIST outputs simultaneously — per-output remux (`flvmux` for rtmp/rtmps, TS
> passthrough for srt/rist), H.264+AAC payload, **no decode, no re-encode, no GPU** anywhere on the receiver host
> unless an output explicitly opts in. v3 adds the **opt-in transcode tier**: an output MAY carry a `transcode`
> object selecting `h264` or `h265`, and the receiver then decodes the arriving elementary stream once and re-encodes
> it for that output only (AV1/H.265 → H.264/H.265). This does not relax the copy-only default — an output with no
> `transcode` block is copied byte-for-byte, exactly as in v2. The 2026-06-05 decode/v4l2loopback/datarhei role
> remains deleted, not deprecated: its fields are **gone, not ignored** (design: `TRANSPORT_PROFILE.md` in the
> product repo; fan-out architecture recorded in `DECISIONS.md`).

There are **two** channels:

1. **Control plane** — HTTP/1.1 REST + JSON (this document, §1–§7). Reliable, acknowledged, request/response.
2. **Telemetry back-channel** — RIST OOB, a fixed 5-byte binary struct (§8). Best-effort, ~1 Hz. **Frozen.**

The media plane (RIST/UDP, MPEG-TS) is described in GSTREAMER.md.

---

## 1. Transport & framing

- Protocol: **HTTP/1.1**. Server: `httplib::Server` (cpp-httplib) in the receiver. Client: `httplib::Client`
  in the encoder.
- Content type: `application/json` for all request and response bodies (except `GET /healthz` response, also JSON).
- Every request and response body carries `"schema_version"` (integer); this build speaks **5** and echoes 5.
- A request whose `schema_version` the receiver does not support is rejected with **400 `invalid_schema`**.
  **`4` is accepted transitionally** (2026-11-08): the encoder lives in a *different* repository
  (`open-broadcast-encoder`) that still sends 4, so refusing it would break every running session. A v4 body
  cannot express `standby`, so it is read with `standby.enabled = false` whatever the body says; there is **no
  other difference between 4 and 5**. This acceptance is to be **removed** once the encoder sends 5. A v1/v2/v3
  body is still rejected — there is no compatibility shim for those.
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
| `outputs[].transcode.codec` | `"h264"`, `"h265"` | `enum class transcode_target : uint8_t` | 0,1,2 |

A receiver MUST reject an unknown enum string with **400 `bad_enum`** naming the offending `field`.
On the wire `transcode.codec` accepts only `"h264"`/`"h265"` (§4): `transcode_target::none` is the internal value
for "no `transcode` block" and is never sent. `"av1"` IS a valid transcode target (YouTube accepts AV1
ingestion over RTMP/RTMPS — see its live encoder settings), but only where the container can carry it:
MPEG-TS (srt/rist). An `av1` target on rtmp/rtmps is rejected as
`bad_enum`. There is no `video.encoder` enum — an output's encoder, when it has one, is implied by
`transcode.codec` and never selected independently. Audio is never declared: AAC passes through to
FLV; a non-AAC audio ES degrades rtmp outputs to video-only (§4 validation) rather than failing them.

---

## 4. POST /start — configure + start

Idempotent on an identical body (returns 200 describing the running session). A second `/start` with a
**differing** body or a different `session_id` while running returns **409 `already_running`**. The operator CLI
values (`--rist-port`, the recovery flags and the standby flags) are applied over the body **before** the
idempotency comparison, so idempotency is judged on the effective config.

### Request body
```json
{
  "schema_version": 5,
  "session_id": "uuid-or-monotonic-string",
  "ingest": { "bandwidth": 8000, "buffer_min": 1000, "buffer_max": 5000,
              "rtt_min": 40, "rtt_max": 500, "reorder_buffer": 30 },
  "source": { "codec": "h264", "width": 1920, "height": 1080, "fps": 60 },
  "standby": { "enabled": true, "bitrate_kbps": 1500,
               "brand_image": "/usr/share/obr/brand.png", "text": "stream standby" },
  "outputs": [
    { "id": "yt",  "type": "rtmp",  "url": "rtmp://a.rtmp.youtube.com/live2", "key_or_streamid": "…" },
    { "id": "fb",  "type": "rtmps", "url": "rtmps://live-api-s.facebook.com:443/rtmp", "key_or_streamid": "…",
      "transcode": { "codec": "h265", "bitrate_kbps": 6000, "gop": 60 } },
    { "id": "cli", "type": "srt",   "url": "srt://203.0.113.9:9000", "key_or_streamid": "clientA" }
  ]
}
```

### Field schema

Top level:
| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema_version` | int | yes | must equal **5**; **4 is accepted transitionally** (§1) and forces `standby.enabled = false` |
| `session_id` | string | yes | opaque; echoed back; used for idempotency/stop matching |
| `ingest` | object | no | RIST listener config; CLI recovery flags are authoritative over it |
| `source` | object | yes | the codec (and, from v5, the geometry) arriving over RIST — see below |
| `standby` | object | no | v5 slate/standby source; **absent ⇒ disabled**. Part of the wire config, so it counts for idempotency — see below |
| `outputs` | array | yes (**MAY be empty**) | empty ⇒ the receiver terminates the RIST stream and (if `--record-dir`) records; a legal "link test / record only" session |

`source` (`codec` is required; the v5 geometry members are optional):
| Field | Type | Default | Bounds | Notes |
|-------|------|---------|--------|-------|
| `codec` | enum codec | — | `h264`/`h265`/`av1` | **required**; the arriving elementary stream |
| `width` | int | 0 | 0–8192 | arriving picture width in pixels; **0 = unknown** (the encoder did not say) |
| `height` | int | 0 | 0–8192 | arriving picture height; **0 = unknown** |
| `fps` | int | 0 | 0–240 | arriving frame rate; **0 = unknown** |

`0` means "unknown" and is deliberately expressible: a slate that assumes 1080p on a 720p session would be a
visible defect, so the receiver is told the geometry or told nothing — it never guesses.

`standby` (every member optional; v5. This is the slate source the receiver holds on air when the live feed is
absent or not yet arriving, so a destination connection is never starved into a reconnect):
| Field | Type | Default | Bounds | Notes |
|-------|------|---------|--------|-------|
| `enabled` | bool | `false` | — | must be a JSON boolean; `"yes"`/`1` are **400 `invalid_schema`**, never coerced |
| `width` | int | 0 | 0–8192 | slate picture width; `0` = follow `source.width` (unknown stays unknown) |
| `height` | int | 0 | 0–8192 | slate picture height; `0` = follow `source.height` |
| `fps` | int | 0 | 0–240 | slate frame rate; `0` = follow `source.fps` |
| `bitrate_kbps` | int (kbps) | 1500 | 100–50000 | encoder target for the slate |
| `max_live_gap_s` | int (s) | 1800 | 0–86400 | longest live gap tolerated before the session is judged dead (0 = no limit) |
| `live_gap_ms` | int (ms) | 1500 | 100–60000 | absence of payload for this long starts the switch-away from live |
| `live_hold_ms` | int (ms) | 500 | 0–10000 | how long live must return and hold before switching back off the slate |
| `max_gap_ms` | int (ms) | 1000 | 100–60000 | longest single gap the slate output may itself leave uncovered |
| `brand_image` | string | `""` | empty **or absolute** path | path **inside the receiver container** (e.g. `/usr/share/obr/brand.png`); a relative path is **400 `invalid_schema`** (`standby.brand_image`) |
| `text` | string | `"stream standby"` | ≤ 64 chars | caption drawn on the slate; longer ⇒ **400 `out_of_range`** |

The object is part of the **wire config**: it participates in `/start` idempotency, so re-posting the same body is a
no-op and changing only the slate is a **different** body (⇒ **409 `already_running`** while a session runs).

#### Standby operator overrides

The container's operator can override the body's standby block from the command line (the node agent reaches these
through `OBR_STANDBY` / `OBR_STANDBY_BRAND`). They are applied to the effective config **before** the idempotency
comparison, exactly like `--rist-port`:

| Flag | Effect |
|------|--------|
| `--standby-slate` | force the slate **on** regardless of the body's `standby.enabled` (including a v4 body, which otherwise forces it off) |
| `--no-standby` | force the slate **off** — the operator kill switch, whatever the body says |
| `--standby-brand <path>` | replace the body's `standby.brand_image` (absolute path inside the container) |
| `--standby-assets <dir>` | directory holding the shipped wordmark `standby-logo.png`; default `/usr/local/share/open-broadcast` |

`--no-standby` and `--standby-slate` are mutually exclusive in intent; if both are given the last one on the
command line wins. A missing `standby-logo.png` or `--standby-brand` file costs the slate that overlay (logged
once), never the render. The entrypoint maps `OBR_STANDBY=0`/`false` to `--no-standby` and anything else (or absent) to
`--standby-slate`.

`outputs[]`:
| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `id` | string | yes | unique within body; echoed in `/status`, `/stats`, logs |
| `type` | enum output_proto | yes | |
| `url` | string | yes | scheme MUST match `type`; validated, **never echoed**; srt/rist URLs require an explicit port |
| `key_or_streamid` | string | no | RTMP stream key / SRT streamid / RIST n/a; **never echoed or logged** |
| `transcode` | object | no | opt-in transcode tier; **absent means copy** (the default). See below. |

Bounds: **max 8 outputs** per session (server-enforced; hosted plans clamp lower at the backplane). Duplicate
`id` ⇒ 400 `bad_enum` field `outputs[].id`; unknown `type` ⇒ 400 `bad_enum`; scheme/type mismatch or unparsable
URL ⇒ 400 `bad_url` naming `outputs[i].url`.

`outputs[].transcode` (all members optional except `codec`; absent `codec` or absent object ⇒ copy-only):
| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `codec` | enum transcode_target | — | **required inside the object**; `"h264"`, `"h265"`, or `"av1"`. `"av1"` is refused on rtmp/rtmps (⇒ 400 `bad_enum`) because flvmux/eflvmux carry no `video/x-av1`; it is valid on srt/rist, whose MPEG-TS does |
| `bitrate_kbps` | int (kbps) | 0 | requested encoder target; `0` = encoder default. From the first full second of ingest the receiver re-derives the target from the rate it measures arriving (`transcode_bitrate_policy`: 90% of the measured rate, floor 1000 kbps, ceiling 24000 kbps) and this value is used only until that measurement exists — see §4.3 |
| `gop` | int (frames) | 0 | encoder GOP length; `0` = encoder default |

A `transcode` object is the **only** way an output opts out of copy-only; `codec` selects the re-encode target and
the receiver decodes the arriving elementary stream **once**, sharing it across every transcode output. An absent or
empty `transcode` block means the output is remuxed/copied, exactly as today.

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
- `schema_version` other than **5** (or the transitional **4**, §1) ⇒ **400 `invalid_schema`**. A v1/v2/v3 body is
  rejected here. When a body declares 4, `standby.enabled` is forced `false` before anything is built.
- Any `ingest.*` numeric out of bounds ⇒ **400 `out_of_range`** naming the field; bad `rist_listen` scheme ⇒
  **400 `bad_url`**.
- Any `standby.*` numeric out of bounds (§4 table) ⇒ **400 `out_of_range`** naming the field (e.g.
  `standby.bitrate_kbps`); a wrong **type** (e.g. `"enabled": "yes"`, `"bitrate_kbps": "2000"`) ⇒ **400
  `invalid_schema`** naming the field; a relative `standby.brand_image` ⇒ **400 `invalid_schema`**. The same
  split applies to `source.width`/`height`/`fps`.
- Unknown `source.codec` / `outputs[].type` / `outputs[].transcode.codec` ⇒ **400 `bad_enum`** naming the field
  (`transcode.codec` accepts only `h264`/`h265`; `"av1"` is `bad_enum`).
- **Egress validation (SSRF/rebinding resistance):** every output host is resolved and vetted — loopback,
  link-local (incl. the cloud metadata range), RFC1918/ULA/CGNAT, multicast and the configured
  `--egress-deny` CIDRs are rejected with **400 `forbidden_destination`** naming the output **id** (never the
  resolved IP). The vetted resolved IP is **pinned**: the output pipeline connects to it and never re-resolves
  **within a connect attempt**. Every reconnect attempt re-resolves and re-vets the name first, so an address a
  platform retires costs one backoff interval rather than the session; an answer the policy refuses is discarded
  and the previous pin is kept. Self-host opt-out: `--egress-allow-private` permits RFC1918/ULA (LAN
  restreaming); loopback/link-local/
  multicast stay blocked. (rtmps connects by hostname — TLS certificate validation is the rebinding defence
  there; see DECISIONS.md.)
- Required mux/sink element absent from the GStreamer registry (`flvmux`, `rtmp2sink`, `srtsink`, `ristsink`,
  `tsdemux`, `h264parse`, `aacparse`, `tsparse`) ⇒ **400 `element_unavailable`** naming the element. An element
  from an **opt-in transcode chain** is probed with alternatives (hardware VAAPI/RADV first, software fallback
  second) and, when no alternative is present, ⇒ **400 `transcode_unavailable`** naming the element, field
  `outputs[i].transcode.codec`. Both are checked synchronously at `/start`. (`encoder_unavailable` does not exist:
  the transcode chain implies its own encoder from `transcode.codec`.)
- **h265 transcode → RTMP/RTMPS is opt-in.** `outputs[].transcode.codec = "h265"` on an rtmp/rtmps output is
  rejected **400 `bad_enum`** (field `outputs[i].transcode.codec`) unless the receiver was started with
  `--allow-enhanced-rtmp`: H.265 requires the Enhanced FLV muxer (`eflvmux`) and platform eRTMP-HEVC support varies. **The chain is currently UNVERIFIED and looks broken** — a live `eflvmux` mux reads back as H.263 from two independent demuxers; the flag defaults off and must stay off until `flv-track-mode` is set correctly *and* a real eRTMP-HEVC destination accepts the stream (see DECISIONS DT-9 / plan §5.2). H.265 is verified over SRT/RIST.
  h264 → RTMP and h265 → SRT/RIST never require the flag.
- **rtmp/rtmps egress needs AVC.** `rtmp_codec_unsupported` (400) fires **only when `source.codec != "h264"` AND at
  least one rtmp/rtmps output has no `transcode` block** — a copy-only rtmp output cannot carry a non-H.264 ES into
  FLV. An **AV1 or H.265 ingest that targets an rtmp/rtmps output MUST carry a `transcode` block** (it cannot be
  copied to RTMP); with one, the receiver decodes and re-encodes to the requested target. If runtime detection later
  finds a non-H.264 video ES on a copy-only rtmp output, that output enters `error` state `rtmp_codec_unsupported`
  (session survives; srt/rist and transcode outputs continue). A non-AAC **audio** ES with rtmp outputs ⇒ those
  outputs run **video-only** and report `audio_dropped: true` rather than failing. The same holds at runtime: an error
  inside an rtmp output's audio branch (decode, resample, AAC encode) retires the branch, not the output.

### Response 200 (started or already-running-identical)
```json
{
  "ok": true,
  "schema_version": 5,
  "session_id": "uuid-or-monotonic-string",
  "state": "running",
  "outputs": [ { "id": "yt", "type": "rtmp" }, { "id": "fb", "type": "rtmps" }, { "id": "cli", "type": "srt" } ]
}
```
The echo carries **id and type only** — URLs and keys are never echoed.

### Error responses
- **400** invalid input:
  ```json
  { "ok": false, "schema_version": 5, "error_code": "invalid_schema|bad_url|bad_enum|out_of_range|forbidden_destination|element_unavailable|transcode_unavailable|rtmp_codec_unsupported", "message": "human readable", "field": "outputs[1].transcode.codec" }
  ```
- **401** unauthenticated: `{ "ok": false, "error_code": "unauthorized" }` (no schema parsing required).
- **409** already running:
  ```json
  { "ok": false, "schema_version": 5, "error_code": "already_running", "session_id": "<current session_id>" }
  ```
- **500** pipeline launch failure:
  ```json
  { "ok": false, "schema_version": 5, "error_code": "pipeline_launch_failed", "message": "<error>" }
  ```

---

### Transcode target follows the measured link

`outputs[].transcode.bitrate_kbps` is a *requested* target, not the rate the encoder is held at for the session.
A portal computes it from destination metadata (codec, resolution, frame rate) and therefore emits the same
number — e.g. ~29860 kbps for a 1440p h265 destination — whatever the originating uplink is carrying.

The receiver measures the one first-hand figure in the path: payload bytes arriving over RIST, sampled once a
second. Once a sample exists, each transcode output's target becomes

```
target = min(90% of the measured rate, 24000 kbps)   -- 1000 kbps floor applies
                                                        only when the link is
                                                        measured above it
```

with the floor yielding to the link when the measured rate is itself below it (asking for more than the link
carries is the failure the policy exists to prevent). A re-target is pushed to the running encoder only when the
new value differs from the applied one by at least 500 kbps **and** 10%, so measurement jitter does not thrash
rate control. No measurement (first second of the session, or a silent window) leaves the requested value in
place; `0` still means the encoder's own default. The wire contract is unchanged: nothing about this appears in
`/status` or `/stats`, and `POST /start` accepts exactly the same body.

---

## 5. POST /stop

### Request body
```json
{ "schema_version": 5, "session_id": "uuid-or-monotonic-string" }
```
Both fields are optional on `/stop` (it is intentionally lenient so a stuck encoder can always halt the stream): an
absent or malformed body is treated as an **unconditional stop**, and `schema_version` is **not** enforced here (unlike
`/start`). If `session_id` is present and does not match the running session ⇒ **409 `session_mismatch`**
`{ "ok": false, "error_code": "session_mismatch", "session_id": "<current>" }`. Stopping when already stopped is a
no-op returning 200.

### Response 200
```json
{ "ok": true, "schema_version": 5, "state": "stopped" }
```

---

## 6. GET /status

Poll for health/state, decoupled from RIST. Recommended encoder poll interval **1–2 s**.

### Response 200
```json
{
  "ok": true, "schema_version": 5, "state": "running", "session_id": "s_9f2c", "uptime_s": 1234,
  "telemetry": { "link_quality": 92, "worst_case_rtt_ms": 140 },
  "outputs": [
    { "id": "yt",  "type": "rtmp",  "state": "running", "connected_s": 1230, "reconnects": 0, "audio_dropped": false, "audio_frames": 3392 },
    { "id": "fb",  "type": "rtmps", "state": "reconnecting", "reconnects": 3, "audio_dropped": false, "last_error": "connection_refused", "transcode": { "codec": "h265", "encoder": "vah265enc", "frames_dropped": 12 } },
    { "id": "cli", "type": "srt",   "state": "running", "connected_s": 1231, "reconnects": 0, "audio_dropped": false }
  ],
  "recording": { "active": true, "bytes": 912345678 },
  "last_bus_error": null
}
```
- A per-output `transcode` object appears **only for outputs that opted into the transcode tier** (§4) and carries the
  target `codec`, the chosen `encoder` element name (e.g. `"vah264enc"`; may be `""` before the first pipeline build)
  and `frames_dropped` (whole-frame drop-oldest on the shared decode frame ring for that output); **copy-only outputs
  omit it entirely**. It never echoes the source codec or any secret. `codec` names the target codec the output is
  re-encoding to (`h264`, `h265` or `av1`) — never `none`, which is what an omitted object means.
- `audio_frames` counts the buffers the muxer's audio pad has received. `audio_dropped: false` with `audio_frames: 0`
  is an audio branch that is linked but has published nothing yet; `audio_dropped: true` means the output is running
  video-only, either because the source carries no AAC or because the branch was retired at runtime.
- **`standby` object.** `GET /status` carries a top-level `standby` object so a panel can show why a
  slate is or is not on air: `{ "enabled": true, "mode": "live|slate|switching", "switches": 3,
  "last_live_ms_ago": 812, "caps_ok": true, "stage_rebuilds": 0, "degraded": "" }` —
  `mode` is the source currently feeding the fan-out, `switches` a monotonic count of live⇄slate transitions,
  `last_live_ms_ago` the age of the last live payload, `caps_ok` whether the slate output negotiated its declared
  caps, `stage_rebuilds` how many times the slate stage had to be rebuilt, and `degraded` is a **string**:
  the reason the slate is running below its request (`"geometry defaulted"`, a render error, `"stage failed to
  start"`) or the **empty string** when it is healthy — it is **not** a boolean, and `enabled` echoes the
  `standby.enabled` the running session was started with (after the operator overrides below).
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
- When stopped: `{ "ok": true, "schema_version": 5, "state": "stopped", "session_id": null, "outputs": [] }`.

---

## 6a. GET /stats (agent-facing)

Polled by the node agent at ~1 Hz (same bearer token — deliberate: agent and encoder share the per-session trust
domain). The handler reads atomics/snapshots only; it MUST NOT take pipeline locks or block the distribution path.

### Response 200 (shape)
```json
{
  "ok": true, "schema_version": 5, "session_id": "s_9f2c", "ts": 1789620000123,
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
  "outputs": [
    { "id": "yt", "state": "running", "bytes_sent": 912345678, "bytes_pushed": 1179648000, "dropped_bytes": 0, "reconnects": 0 },
    { "id": "fb", "state": "running", "bytes_sent": 909000000, "bytes_pushed": 914408192, "dropped_bytes": 0, "reconnects": 0, "transcode": { "codec": "h265", "encoder": "vah265enc", "frames_dropped": 12 } }
  ],
  "recording": { "active": true, "bytes": 912345678, "dropped_bytes": 0 }
}
```
- `outputs[].transcode`: present only for outputs that opted into the transcode tier, carrying the target `codec`,
  the chosen `encoder` element name and `frames_dropped` (whole-frame drop-oldest on the shared decode frame ring);
  **copy-only outputs omit it entirely**. The decoder stage decodes the ingest elementary stream once and publishes
  raw frames to that ring; each transcode output pulls with its own cursor and reports its own drops.
- `rist.peers[]`: one entry per connected RIST peer — **each link in a bond arrives as a separate peer**, so this
  is the per-WAN-path view. Per-peer counters come from the vendored librist's `rist_stats_receiver_peer`
  (verified present).
- `ring_size_bytes`: the fan-out ring chosen at `/start` — `clamp(10 s × ingest.bandwidth, 16 MiB, 256 MiB)`.
- `outputs[].dropped_bytes`: ring drop-oldest accounting for that consumer (slow-output back-pressure never
  reaches ingest). `recording` appears when a recorder exists.
- `outputs[].bytes_sent`: bytes the destination sink took on its sink pad — post-muxer container payload, the
  counter that tracks the publish socket's egress (and therefore the interface's tx). For a transcode output it is
  **far smaller** than the decoded-frame volume; it never includes raw frames or ingest bytes.
- `outputs[].bytes_pushed`: bytes pushed into that output's appsrc(s) — raw decoded frames for a transcode output,
  compressed access units for a copy output. It is a feed-diagnostic superset of `bytes_sent`, not an egress
  counter; the two are reported under distinct names and must never be conflated.
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
