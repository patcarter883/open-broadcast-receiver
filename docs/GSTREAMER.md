# GSTREAMER.md — receiver fan-out pipeline templates (transport profile)

Date: 2026-07-18. Verified pipeline templates for the copy-only fan-out receiver: one incoming RIST/TS stream,
one **independent** GStreamer pipeline per output over the in-process SPMC ring (see `DECISIONS.md` — fan-out
architecture; wire contract in `CONTRACT.md`). No decode, no encode, no GPU elements anywhere.

Template verification status: the RTMP template was verified live against SRS 5.0.213 (`start publish`,
AAC accepted) and the SRT template against an `srtsrc` listener (byte-aligned TS delivered) on 2026-07-18.

---

## 0. Element availability

All elements ship in Debian/Ubuntu GStreamer packages ≥ 1.24 (project floor 1.28 for production):

| Element | Package | Used for |
|---------|---------|----------|
| `appsrc` | base (gst-app) | ring → pipeline injection, one per output |
| `tsparse` | bad | TS alignment (7×188) + timestamping |
| `tsdemux` | bad | RTMP path: ES extraction |
| `h264parse` | bad | byte-stream → AVC for FLV |
| `aacparse` | good | ADTS → raw AAC for FLV |
| `flvmux` | good | RTMP container |
| `rtmp2sink` | bad (rtmp2 plugin) | RTMP/RTMPS publish (native TLS, async connect) |
| `srtsink` | bad | SRT caller output |
| `ristsink` | bad | RIST output |

Presence is checked synchronously at `/start` → **400 `element_unavailable`** naming the element.

---

## 1. Input chain (RISTNetReceiver → SPMC ring)

There is **no input pipeline**. The RIST `networkDataCallback` memcpys each payload into the SPMC ring and
returns — the producer **never blocks** and ingest can never be back-pressured by a slow output (the old
`block=true` appsrc coupling is gone; each output's own appsrc still uses `block=true` internally, which bounds
only that output's feeder thread). Slow consumers take per-consumer drop-oldest at TS-packet boundaries with
`dropped_bytes` accounting (surfaced in `GET /stats`).

Ring sizing: `clamp(10 s × ingest.bandwidth, 16 MiB, 256 MiB)`, allocated at `/start`.

---

## 2. TS passthrough template (srt / rist outputs)

```
appsrc name=osrc is-live=true do-timestamp=true format=time block=true max-bytes=4194304
  ! tsparse alignment=7
  ! srtsink name=osink wait-for-connection=false
```

- `ristsink name=osink` for rist outputs (`address`/`port` properties).
- The sink is configured via `g_object_set` after parse — **the parse string carries no URLs and no secrets**
  (pipeline strings are loggable by definition). SRT `streamid` is set as a property, not in the URI.
- The connect target is the **pinned, vetted IP** from egress validation — never a hostname, never re-resolved.

Open decision (FIXPLAN M1.5): `do-timestamp` + synced sink re-paces on arrival times; verbatim bytes with
`sync=false` may be strictly better for TS passthrough. Bench both over a 10-minute capture (PCR interval
jitter) before locking this template; the loser gets recorded here as rejected with the measurement.

---

## 3. RTMP/RTMPS template (per output)

```
appsrc name=osrc is-live=true do-timestamp=true format=time block=true max-bytes=4194304
  ! tsparse set-timestamps=true alignment=7
  ! tsdemux name=d
  d. ! queue ! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! mux.
  d. ! queue ! aacparse ! audio/mpeg,mpegversion=4,stream-format=raw ! mux.
  flvmux name=mux streamable=true latency=1000000000 ! rtmp2sink name=osink async-connect=true
```

Connection properties set programmatically (never in the parse string): `scheme`, `host`, `port`,
`application` (URL path), `stream` (the key). Plain rtmp connects to the **pinned IP**; rtmps connects by
**hostname** — TLS certificate validation against the real hostname is the rebinding defence for TLS
destinations, and SNI/verification break on a bare IP (recorded in DECISIONS.md).

---

## 4. Recording consumer

Not a pipeline: a dedicated thread copies its ring cursor through a 1 MiB buffer into
`<record-dir>/<session_id>.ts` via `write(2)`, `fdatasync` every 5 s. Verbatim bytes — what went in is what you
download. A stalling disk makes this consumer take ring drops; it can never touch the stream.

---

## 5. Pitfalls (ranked — the ones that cost real debugging time)

1. `flvmux streamable=true` is mandatory for live (no seekable header rewrite).
2. FLV requires **AVC** stream-format and **raw** AAC: `h264parse` converts byte-stream→avc using in-band
   SPS/PPS (the encoder muxes with `config-interval=1`, so config is present ≤1 s into any join point);
   `aacparse` converts ADTS→raw. The caps filters make the conversion explicit and fail loudly if impossible.
3. Joining mid-stream: a new/reconnected output starts at an arbitrary ring position; `tsdemux` waits for
   PAT/PMT, `h264parse` for SPS/PPS+IDR. Worst-case output start latency ≈ one GOP + 1 s. Accepted for v1; a
   keyframe-index on the ring (start consumers at the last PAT/PMT+IDR) is a cheap v2 latency win.
4. `rtmp2sink` (rtmp2 plugin) over legacy `rtmpsink` (librtmp): actively maintained, native RTMPS, async
   connect. Behaviour note from live testing: with no media flowing yet, some ingests (SRS included) close the
   idle connection — the output object's retry-forever backoff rides this out until media arrives; do not treat
   early "connection closed remotely" as fatal.
5. A platform ingest closing TCP on a bad stream key is indistinguishable from a transient reset at the sink's
   error surface — which is exactly why the reconnect policy retries everything except
   `rtmp_codec_unsupported` (see CONTRACT §6).
6. `tsdemux` pads are sometimes-pads; the `d. !` links above are deferred-linked by `gst_parse_launch`. A
   non-AAC audio pad simply never links (video-only output, `audio_dropped: true`) — tsdemux's flow combiner
   tolerates the unlinked branch as long as video flows.
7. Each pipeline runs its own bus handling on the output object's thread; no shared `GMainLoop` across outputs
   (this is what makes per-output errors local by construction).
8. The producer (RIST thread) writes the ring even while outputs rebuild — a reconnecting output rejoins live
   at the current head, it does not replay backlog.
