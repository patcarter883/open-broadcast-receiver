# DECISIONS.md — open-broadcast-receiver redevelopment

Date: 2026-05-30
Authors: synthesis architect (verified against encoder + ndi-rist-server + rist-cpp sources)
Status: ACCEPTED. The control-plane decision below is FINAL per the judge; everything else is built around it.

This document records every significant design decision for the new **headless** `open-broadcast-receiver`
(`/home/pat/Projects/open-broadcast/open-broadcast-receiver`, empty git repo, branch `main`, no commits) and the
required changes to the existing `open-broadcast-encoder`. Each decision states the choice, the rationale, and the
alternatives that were rejected. The companion documents are CONTRACT.md (wire format), GSTREAMER.md (verified
pipeline templates) and IMPL_PLAN.md (file tree + change-list).

---

## 0. Context (verified, not assumed)

The following were confirmed by reading the actual files on this box, not from the project summary:

- `wan_telemetry` is `struct __attribute__((packed)) { uint8_t link_quality; uint32_t worst_case_rtt; }` with a
  `static_assert(sizeof(wan_telemetry) == 5)` — `source/lib/lib.h:53-59`.
- `rist_oob_cb` hard-gates on `size != sizeof(wan_telemetry)` (i.e. exactly 5) then `memcpy` + `ntohl(worst_case_rtt)`
  — `source/main.cpp:77-90`.
- The encoder is the RIST **caller**: URL `rist://{host}:{port}?bandwidth=..&buffer-min=..&buffer-max=..&rtt-min=..&rtt-max=..&reorder-buffer=..&timing-mode=2`, **no `@`**, ports step `port + 2*i`, `mProfile = RIST_PROFILE_ADVANCED` — `source/transport/transport.cpp:88-109`.
- Enums are `enum class codec : uint8_t { h264, h265, av1 }` and `enum class encoder : uint8_t { amd, qsv, nvenc, software }` — `source/lib/lib.h:26-39`. **Order is the wire contract for the JSON int mapping.**
- `kEncoderTemplates[encoder][codec]` fragments use `name=videncoder bitrate={}` and terminate in a parse element with `config-interval=1` (except av1) — `source/encode/encode.cpp:197-253`.
- cpp-httplib **0.40.0** is at `/mnt/data/projects/llama.cpp/vendor/cpp-httplib/httplib.h`; `set_bearer_token_auth`, `set_pre_routing_handler`, `set_payload_max_length`, `set_read_timeout`, `set_keep_alive_timeout` all present.
- nlohmann/json **3.12.0** is system-installed at `/usr/include/nlohmann/json.hpp` with a CMake config package.
- The reference `ndi-rist-server/main copy.cpp` uses `RISTNetReceiver` + appsrc with caps `video/mpegts,systemstream=true,packetsize=188` but `mProfile = RIST_PROFILE_MAIN` and an rpclib server on `0.0.0.0:5999`.

---

## 1. Control plane: REST/JSON over HTTP (cpp-httplib + nlohmann/json) — FINAL

**Decision.** The receiver runs an `httplib::Server`; the encoder runs an `httplib::Client`. Bodies are
nlohmann/json. Every request carries `Authorization: Bearer <token>` (mandatory shared secret). Endpoints:
`POST /start` (configure+start), `POST /stop`, `GET /status`, `GET /healthz`. All bodies are versioned with
`"schema_version": 1`. The control plane is **separate from the media path** — see §3.

**Rationale (the dependency math and debuggability decide it):**

1. **Zero new linked libraries.** cpp-httplib is header-only (needs only pthread, already supplied by
   `Threads::Threads` in both repos) and nlohmann/json is system-installed with a CMake config. The entire
   control-plane footprint is *two headers, zero ExternalProject builds*.
2. **The opposite extreme is unbuildable here.** rpclib is **not** on the box (`/usr/include/rpc` is glibc `netdb.h`
   only; no `rpc/server.h`, no pkg-config, no vcpkg). Using it would mean vendoring an unmaintained 2.3.0 tree that
   internally pins `CMAKE_CXX_STANDARD 14`, built under gcc 16 — the single largest unquantified risk for a
   one-controller command bus.
3. **The new payload is nested and versioned.** The redesign requires an *array of outputs*, each with type
   (rtmp/srt/rist), url, key/streamid, params, and per-stream video/audio copy-vs-reencode + codec/encoder/bitrate/upscale.
   Self-describing JSON models this cleanly and additively. The original receiver's flat positional
   `MSGPACK_DEFINE_ARRAY RpcData` (field *order* is the wire contract, single rtmp destination) is exactly the shape
   that silently corrupts when a field is added.
4. **Headless cloud bring-up.** curl-debuggable, line-oriented HTTP with standard status codes (200/400/401/409/500)
   is materially better for an unattended VPS than binary msgpack-rpc on TCP 5999.
5. **The classic HTTP weakness is neutralized here.** Real-time receiver health already arrives over the RIST OOB
   channel (the 5-byte `wan_telemetry`). The control plane only needs request/response configure/start/stop plus a
   pull `GET /status`, so HTTP's polling model is sufficient, and `GET /status` gives the encoder a second,
   media-independent way to detect a wedged receiver.
6. **It fits the repo conventions.** C++20, `Threads::Threads` already linked, RAII/ASan per CLAUDE.md. Software-only
   GStreamer paths on this box validate the full control + pipeline-assembly flow even though only x264/x265/avenc_aac
   are locally testable.

**Alternatives rejected:**

- **rpclib + msgpack-rpc (Proposal 1).** Not on the box; unmaintained, C++14-pinned, unverified under gcc 16. Binary
  on TCP 5999 is not curl/nc-debuggable; no built-in auth/TLS (original bound `0.0.0.0:5999` wide open). RPC's
  synchronous-status advantage is moot because async health already arrives via RIST OOB.
- **Length-prefixed JSON over raw TCP (Proposal 3, runner-up).** Identical zero-new-dependency footprint and correct
  reliable/ordered TCP semantics, but you hand-roll framing, partial-read/short-write handling, max-length
  allocation-DoS guards, socket timeouts, and auth — surface cpp-httplib already provides as library primitives
  (routing, keep-alive/timeout limits, `set_bearer_token_auth`). For one vendored header you remove that whole class
  of hand-rolled bugs and gain curl/Postman debuggability and a TLS compile flag. Choose this only if vendoring even
  one header were unacceptable.
- **Reuse the RIST OOB channel for control.** Wrong substrate. `sendOOBData` is a single `rist_oob_write` with no
  fragmentation/ACK/ordering and is "not protected for network loss"; an OOB write failure can tear down live media
  (see §6 wrapper bug). The encoder already hard-gates OOB to exactly 5 bytes, so the channel is semantically claimed
  by telemetry; a multi-destination configure payload is hundreds of bytes to low-kB with no reassembly layer.
- **gRPC.** Unavailable (no libgrpc, no grpc_cpp_plugin, no vcpkg) and oversized; its strengths are irrelevant for one
  trusted encoder controlling one receiver.

---

## 2. Endpoints, idempotency, and single-controller model

**Decision.** `POST /start` configures and starts in one call; it is idempotent on an identical body (returns 200
describing the running session) and rejects a *second concurrent, differing* session with **409 `already_running`**.
`POST /stop` stops; `session_id` is optional but if present and mismatched returns 409. `GET /status` returns pipeline
state plus telemetry mirrored from the OOB channel. `GET /healthz` is a token-gated liveness probe.

**Rationale.** The receiver serves exactly one encoder. Combining configure+start avoids a partial-config state
machine. Idempotency lets the encoder retry a flaky `POST /start` safely. 409 (not 400) makes "already running"
distinguishable from a malformed request so the encoder can decide to `stop` then re-`start`.

**Rejected:** separate `/configure` + `/start` (extra round trips, partial-state hazard on an unattended daemon);
allowing concurrent sessions (one receiver = one pipeline = one bus, see GSTREAMER.md §1).

---

## 3. Media path unchanged; RIST listener mirrors the caller

**Decision.** The media path stays on RIST/UDP exactly as today. The receiver **listens** with one RIST URL per
stream: `rist://@[::]:PORT?...` (or `rist://@0.0.0.0:PORT?...`). It mirrors the encoder's recovery params and
**`timing-mode=2`** in the URL, and sets `mProfile = RIST_PROFILE_ADVANCED`.

**Rationale.**
- The encoder is the caller (no `@`), so the receiver must listen (`@` makes librist `initiate_conn=0`).
- **Profile must match.** Encoder = ADVANCED (verified transport.cpp:106). The reference receiver used MAIN — this is
  the single load-bearing RIST change. SIMPLE has no OOB channel at all; MAIN vs ADVANCED differ in the control-channel
  feature set the bidirectional OOB telemetry depends on. Mismatch prevents the GRE/RTCP handshake from converging.
- **`reorder-buffer` must be carried in the URL.** The rist-cpp wrapper copies most of `mPeerConfig` into
  `mRistPeerConfig` in `initReceiver` but **omits `recovery_reorder_buffer`**, so the only reliable way to set it on
  the receiver is the URL query string. Put `buffer-min/max`, `rtt-min/max`, `reorder-buffer`, `timing-mode=2` all in
  the listen URL (and set the struct fields as sane defaults).
- **No PSK.** The encoder sets no `mPSK`/`mCNAME`; the receiver must leave them empty or the AES handshake fails
  one-sided.
- **Multi-stream:** the encoder uses `port + 2*i`; add one listen URL per stream with the matching port.

**Rejected:** changing the media transport to SRT/RTP-only (breaks the encoder, discards the working RIST recovery and
OOB back-channel); setting reorder-buffer only via the struct (silently ignored by the wrapper).

---

## 4. Bidirectional OOB telemetry: receiver implements the encoder's existing 5-byte contract

**Decision.** The receiver computes `link_quality` (0–100) and `worst_case_rtt` (ms) from `rist_stats` on each ~1 Hz
`statisticsCallback` and sends exactly 5 bytes back to the encoder peer via `RISTNetReceiver::sendOOBData`, in the
byte-identical layout the encoder already consumes. The same values are cached into receiver state for `GET /status`.

**Rationale.** The encoder already registers `networkOOBDataCallback` and expects this struct; the original receiver
implemented *no* telemetry. On a receiver, the populated stats arm is `stats.stats.receiver_flow`
(`stats_type == RIST_STATS_RECEIVER_FLOW`) — **not** `sender_peer`, which is what the encoder reads on its side.
`receiver_flow.quality` is `received*100/(received+missing)` (0–100 double), matching the 0–100 semantics the
encoder's `scale_encoder_bitrate` drives its adaptive loop with. `worst_case_rtt` = `receiver_flow.rtt` (ms),
optionally maxed over `receiver_flow.peers[].rtt` for a conservative worst-case.

Wire packing must be explicit to avoid any ABI/packing surprise:
```c
uint8_t pkt[5];
pkt[0] = quality_0_100;            // link_quality (raw byte, no byte order)
uint32_t be = htonl(rtt_ms);       // worst_case_rtt, network order
memcpy(&pkt[1], &be, 4);           // send exactly 5 bytes
```
The encoder does `ntohl`. Send one packet per stats tick aligned to the ~1 Hz `statisticsCallback`.

**Rejected:** sending raw host-order ints (encoder would misread RTT); sending the full packed struct pointer (works
but the explicit buffer removes any tail-padding doubt); reading `sender_peer` on the receiver (wrong union arm,
garbage values).

---

## 5. Copy-by-default with optional reencode and multi-destination

**Decision.** The receiver supports three video dispositions per output: **copy** (passthrough h264/h265/av1 parse →
no decode), **reencode** (decode → optional upscale → encode via the `[encoder][codec]` template matrix), each output
selectable independently. Audio is copied (aacparse) by default and reencoded (avdec_aac → avenc_aac) only when an
output requests it. Multiple destinations fan out from a `tee` in a single pipeline. This replaces the original
receiver, which **always** reencoded video and **always** copied audio to a **single** rtmp destination.

**Rationale.** Copy is cheap and lossless and is the common case (the encoder already produced the target codec);
reencode is for transcode/upscale/codec-bridging. Independent per-output disposition is what enables, e.g., copying
h265 to SRT while reencoding to h264 for an RTMP CDN (see §5a). One pipeline = one bus = unified EOS/ERROR/teardown
(GSTREAMER.md §1).

### 5a. RTMP codec constraint is enforced in control logic

**Decision.** `flvmux` (all RTMP output) accepts **only** H.264(avc)+AAC(raw) — verified from sink-pad caps on this
box (no H.265, no AV1). When an output is `type=rtmp|rtmps` and the effective video codec is h265/av1, the receiver
**must** either (a) reject that output with a 400 `bad_enum`/`out_of_range`-class error naming the field, or (b) force
an H.264 reencode sub-branch *just for the RTMP leg* (decode → x264enc/nvh264enc → flvmux) while other outputs copy
the original codec.

**Decision for v1:** **reject** with a clear control-plane error (`error_code: "rtmp_codec_unsupported"`, field
`outputs[i].video.codec`). The forced-reencode path is documented in GSTREAMER.md as a v2 option but is not auto-applied
in v1 to keep behaviour predictable.

**Rationale.** Generating an unparseable/uncombinable pipeline and failing at `gst_parse_launch` is worse on a headless
box than a precise 400 the encoder can surface to the operator.

---

## 6. rist-cpp wrapper: vendor as-is, but patch the OOB-failure teardown

**Decision.** Reuse the encoder's exact `external/rist-cpp` checkout (git submodule or copy) and its ExternalProject
block. **Patch** `RISTNetReceiver::sendOOBData` so a non-zero `rist_oob_write` return **logs and returns false**
instead of calling `destroyReceiver()`.

**Rationale.** As shipped, a single transient OOB telemetry write failure would tear down the entire receiver and
the live media path. Telemetry is best-effort; its failure must never nuke media. Also gate the first OOB send on an
established connection (a captured peer from `networkDataCallback`/`getActiveClients`) and null-check the peer, because
calling before the handshake completes returns an error. The "OOB currently not working in librist" comment in
RISTNet.h is stale for this build — the encoder already consumes these packets in MAIN/ADVANCED.

**Rejected:** relying on the unpatched wrapper (couples telemetry failure to media teardown); calling `rist_oob_write`
directly on a raw ctx (the wrapper does not cleanly expose it; patching the one method is smaller and localized).

### 6a. Telemetry fallback over the control plane (documented, not v1-default)

If OOB proves unreliable on the deployment box, the receiver can POST `link_quality`/`worst_case_rtt` as a tiny JSON
heartbeat to an encoder-side endpoint at the same 1 Hz, feeding the same code path `rist_oob_cb` uses. v1 keeps OOB as
primary; the HTTP fallback is a documented contingency, not built by default.

---

## 7. Headless: no FLTK, no NDI; lifecycle entirely driven by REST

**Decision.** The receiver has no GUI and no NDI dependency. `main` does `gst_init`, parses
`--control-port`/`--rist-port`/`--token`/`--help`, constructs `app_context`, wires control handlers to
receive+restream lifecycle, starts the control server on a background thread, then blocks on a SIGINT/SIGTERM-driven
wait. It is idle until `POST /start`.

**Rationale.** Target is virtualised/cloud servers. All control comes from the encoder. Dropping FLTK/NDI removes the
two heaviest external builds and platform deps.

---

## 8. Dependency strategy (no vcpkg in either repo for these libs)

**Decision.**
- **cpp-httplib:** vendor the single header into each repo's `external/httplib.h` (copy of the verified 0.40.0 file),
  pin the version, add `<repo>/external` to the relevant target include dirs, `#include "httplib.h"`. Plain HTTP needs
  only pthread (`Threads::Threads`); no OpenSSL define for v1.
- **nlohmann/json:** `find_package(nlohmann_json 3.2 REQUIRED)` against the system package; link
  `nlohmann_json::nlohmann_json` on the modules that (de)serialize. Do **not** vendor — the system package is
  authoritative.
- **rist-cpp:** unchanged ExternalProject, exposing IMPORTED `rist`/`ristnet`.
- **GStreamer:** `pkg_search_module(... IMPORTED_TARGET ...)`, `PkgConfig::gstreamer*`, no vendoring.

**Rationale.** Matches what is verified present on the box and the encoder's existing patterns; zero new build systems.

**Risk — vendored header drift.** Both repos carry a copy of httplib.h. Pin/version it (record `CPPHTTPLIB_VERSION
"0.40.0"`), keep the two copies byte-identical, and update them together.

---

## 9. Security / auth

**Decision.**
- Mandatory `Authorization: Bearer <token>` on **every** route, including `/status` and `/healthz`, enforced via
  `set_pre_routing_handler` with a constant-time compare; reject with 401 otherwise.
- Token delivered out-of-band (env/config, never committed). Redact the token and `key_or_streamid` in all logs and
  in `GET /status` output.
- Bind to a private/management interface, or `0.0.0.0` only behind a cloud security-group source-IP allowlist /
  WireGuard. Expose publicly only with TLS via `-DCPPHTTPLIB_OPENSSL_SUPPORT` (OpenSSL present).
- **Server-side validation before any `gst_parse_launch`:** URL scheme allowlist (`rtmp`/`rtmps`/`srt`/`rist` only).
  **Every** interpolated value is single-quoted in the launch string, so `is_pipeline_safe` bans only the
  quote-escape set (`'`, `"`, `\`, control chars) — deliberately *not* `&`/`?`/`=`, which occur in valid SRT/RIST
  query URLs. Because the RIST `ristsink address=` is a bare token, `validate_config` additionally requires the
  `srt`/`rist` authority to parse and the host to be a clean token (`[A-Za-z0-9.:_\-\[\]]`), and the RIST port to be
  even (RTCP uses `port+1`). Clamp `bitrate`/`width`/`height`/`port`/buffers to bounds; range-check codec/encoder
  enums. (This refinement — quote-everything + host-token + even-port — closed an `address=` injection gap found in
  adversarial review.)
- Set read/write/keep-alive timeouts and `set_payload_max_length` so one stuck or malicious client cannot exhaust
  threads; treat as single-controller and reject the second concurrent `/start` with 409.

**Rationale.** `POST /start` can stream anywhere and `POST /stop` can DoS the broadcast; the body carries RTMP keys
/ SRT streamids and the header carries the bearer token. Pipeline-string injection via `url`/`key_or_streamid` is the
top risk. These mitigations are all library primitives or cheap string checks.

**Rejected:** unauthenticated `/status` or `/healthz` (information disclosure / liveness probing of a control daemon);
trusting the client to sanitize URLs.

---

## 10. Repo layout

**Decision.** Mirror the encoder's modular layout, minus FLTK/NDI:
`source/lib` (shared types + JSON conversion), `source/receive` (RISTNetReceiver wrapper + OOB telemetry),
`source/restream` (GStreamer pipeline manager — the analogue of `encode/`), `source/control` (httplib REST server),
`source/main.cpp`. CMake mirrors the encoder: `cmake/prelude.cmake`, `cmake/variables.cmake`,
`cmake/ExternalBuilds.cmake` (rist-cpp only). See IMPL_PLAN.md for the full tree.

**Rationale.** Lowest cognitive load for engineers moving between the two repos; reuses the encoder's proven CMake
patterns and teardown discipline.

---

## 11. Bug fixes carried over

- **Listener URL `&` bug:** the original ndi-rist-server emitted `bandwidth={}buffer-min=` with a missing `&`. The new
  `build_listener_url` emits `bandwidth={}&buffer-min=...`.
- **Profile:** MAIN → ADVANCED (see §3).
- **`profile=` URL param:** this librist build rejects an unknown `profile` URL parameter and fails the whole listener;
  the listener URL carries only `timing-mode=2`, and the ADVANCED profile is set via `mProfile`.
- **OOB teardown:** patch `sendOOBData` (see §6).
- **Receiver teardown order:** `destroyReceiver()` is called *before* clearing callbacks (and without a prior
  `closeAllClientConnections()`), so the librist stats thread cannot race peer destruction via `sendOOBData`.
- **`ristsink address=` injection / even port / secret redaction:** see §9 — found and fixed in adversarial review.

---

## 12. Open questions deferred (not blocking v1)

1. Whether `worst_case_rtt` should be `receiver_flow.rtt` (avg over live peers) or `max(peers[].rtt)`. v1 uses
   `receiver_flow.rtt` with the max-over-peers refinement noted in GSTREAMER/IMPL plans.
2. Whether the encoder should auto-derive the receiver RIST listener port from `output_cfg`. v1 keeps control-host and
   RIST-host independent in the UI but recommends deriving.
3. Whether TLS is mandatory for the deployment topology. v1 ships plain HTTP + token; TLS is a compile flag for public
   hops.
