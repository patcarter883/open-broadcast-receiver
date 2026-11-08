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
appsrc name=osrc is-live=true do-timestamp=true format=time block=false max-bytes=4194304
  ! tsparse set-timestamps=true alignment=7
  ! tsdemux name=d
  queue name=vq ! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! mux.
  queue name=aq ! aacparse ! capsfilter name=acaps caps=audio/mpeg,mpegversion=4,stream-format=raw
  flvmux name=mux streamable=true latency=1000000000 ! rtmp2sink name=osink async-connect=true
  funnel name=ddrop ! fakesink sync=false async=false
```

The two parser branches are not linked from the parse string. `tsdemux` announces its
src pads in PMT stream order, so a template link would put whichever pad arrives first
into whichever parser it faces; instead `output::demux_pad_added` links each announced
pad by its caps — `video/x-h264` to `vq`, AAC to `aq`, everything else (non-H.264
video, other audio codecs, private/subtitle streams) to the `ddrop` funnel. Every
announced pad therefore gets a peer, which is what keeps `tsdemux` from failing the
pipeline with `streaming stopped, reason=not-linked`.

The copy path's audio branch also stops at `capsfilter name=acaps`, so both tiers
request the muxer's audio pad the same way: `attach_audio_branch()` links it when an
AAC pad is announced and sinks it otherwise. A copy-only output fed a video-only TS
therefore publishes on its first frame, and no timer decides whether that happens.

Connection properties set programmatically (never in the parse string): `scheme`, `host`, `port`,
`application` (URL path), `stream` (the key). Plain rtmp connects to the **pinned IP**; rtmps connects by
**hostname** — TLS certificate validation against the real hostname is the rebinding defence for TLS
destinations, and SNI/verification break on a bare IP (recorded in DECISIONS.md).

Transcode variant (§7): encoded video comes from the shared frame ring (`appsrc name=osrc` → queue → pinned
NV12 → encoder → parser → `mux.`) and AAC audio is copied from the ts_ring by a second branch
(`appsrc name=asrc ! tsparse ! tsdemux name=ad ! queue name=aq ! aacparse ! capsfilter name=acaps`). That
branch **stops at the capsfilter**: the muxer's audio pad is requested from `demux_pad_added()`, i.e. only
when the source TS actually exposes an audio pad. A video-only ingest therefore hands `flvmux` a video pad
alone and publishes on the first encoded frame; nothing time-based decides whether video flows.

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
6. `tsdemux` pads are sometimes-pads announced in PMT order, so pad-to-parser routing is done from the pad's
   caps at announce time (`video/x-h264` → `h264parse`, MPEG-2/4 audio → `aacparse`), and every pad that no
   branch handles is linked to the drop funnel. A pad left unlinked fails the pipeline with
   `streaming stopped, reason=not-linked`; a pad handed to the wrong parser fails negotiation. Non-AAC audio
   and non-H.264 video still publish video-only, with `audio_dropped: true`.
7. Each pipeline runs its own bus handling on the output object's thread; no shared `GMainLoop` across outputs
   (this is what makes per-output errors local by construction).
8. The producer (RIST thread) writes the ring even while outputs rebuild — a reconnecting output rejoins live
   at the current head, it does not replay backlog.

---

## 6. AV1-in-MPEG-TS fixture (decode acceptance)

The decode stage is exercised without a rig by one committed command, which both
builds the fixture and runs the stage over it:

```
make fixture-decode
```

`make fixture` alone writes `build/av1-fixture.ts`; `test/tools/make-av1-ts-fixture.sh`
is the generator (see its header for the encoder settings it mirrors).

**Why the fixture is generated and not hand-made.** MPEG-TS has no standardised
stream type for AV1. `mpegtsmux` refuses AV1 by default — *"Failed to determine
stream type or mapping is not supported ... you may have to set the
enable-custom-mappings property to TRUE"* — so the only representative fixture
is one muxed the way the product's encoder muxes it
(`open-broadcast-encoder`, `pipeline_build_sink`):

```
mpegtsmux alignment=7 enable-custom-mappings=true
```

after `av1parse ! video/x-av1,stream-format=obu-stream,alignment=frame`. The
resulting PMT carries a private stream (stream type `0x06`, registration `AV1G`),
which `tsdemux` maps to `video/x-av1`. A TS muxed *without* the flag has no AV1
mapping, `tsdemux` exposes no video pad, and the chain dies with *"This stream
contains no valid or supported streams"* — a synthetic TS like that tests the
fixture, not the decoder.

`decode_file` feeds the fixture through a real `ts_ring` in 1316-byte RIST
datagrams and prints the stage's own counters; `frames=` is the measurement.

---

## 7. Decoder selection: hardware first, and negotiated on the decoder's terms

The decode stage does not commit to one decoder. `transcode_decoder_alternatives()`
(`lib.cpp`) lists, per source codec, the hardware decoder first and the software
decoder for the same codec second — `{vaav1dec, av1dec}`,
`{vah265dec, avdec_h265}`, `{vah264dec, avdec_h264}` — and `transcode_decoder_chain()`
keeps the entries the registry actually has, in that order.

**The parser's output is pinned to the decoder's own sink caps.** A decoder
declares the layout it takes on its sink pad, and the two AV1 decoders disagree:

| element | sink caps it fixes |
|---|---|
| `vaav1dec` (VAAPI, hardware) | `profile=main`, `alignment=frame` |
| `av1dec` (software) | `stream-format=obu-stream`, `alignment=tu` |

`av1parse` accepts all of `alignment={obu,tu,frame}` on its src pad and, left
alone, produces **`alignment=tu`** — the software decoder's layout. Frame
alignment, the only thing `vaav1dec` takes, then exists on the branch only if
something downstream constrains the negotiation to it. That is a race against
however the branch happens to be linked, and the wrong outcome is a hardware
decoder that is fed a layout it refuses (`not-negotiated (-4)`), a pipeline that
stays `ASYNC`, an empty frame ring and an output that never sends a byte.

`decoder_branch_caps()` (`fanout/decoder.cpp`) reads the decoder's sink template
from the registry, keeps the fields it fixes to a single value, and the template
puts them on a caps filter between the parser and the decoder:

```
queue name=vqueue ! av1parse ! video/x-av1,profile=main,alignment=frame ! vaav1dec ! ...
queue name=vqueue ! av1parse ! video/x-av1,stream-format=obu-stream,alignment=tu ! av1dec ! ...
```

The caps come from the element, so they cannot drift from what the installed
decoder declares. Fields the element lists or leaves as ranges (`profile` on
`vah264dec`, `width`) are not pinned. A queue forwards a caps query upstream, so
a stream that cannot satisfy the pin is refused at the demux pad — before it can
wedge the pipeline — and the attempt is retired at once rather than after the
no-frame window.

`decoder` builds the **first** chain entry. That attempt is retired, and the next
entry built in its place, when any of these happens:

- the pinned caps cannot be met (the parser's caps event, or the demux pad's
  caps, do not intersect the decoder's sink caps) — this is the precise form of
  what the bus only calls `not-negotiated`,
- the attempt posts a bus **error**,
- the attempt is fed TS bytes and its `tsdemux` produces **no video buffer within
  5 s** of the first fed chunk, or
- the attempt is handed video buffers and produces **no decoded frame within 5 s**
  of the first one — the bound for a decoder that wedges without saying anything,
  leaving the pipeline in `ASYNC`.

Both timed windows are armed by data arriving, never by the pipeline being built.
A session's stream starts seconds after `/start` (RIST holds the first chunks
until the listener is up), so a clock started at build would retire a healthy
hardware decoder before its first buffer and silently run the session on the
software one. The no-frame clock also covers a first GOP that begins mid-stream:
`tsdemux` hands over buffers from wherever the listener joined, and the decoder
cannot emit before the next keyframe.

The failure is logged once, naming the element, the stream's real caps and the
field the decoder refused:

```
decoder vaav1dec failed: vaav1dec cannot take this stream (video/x-av1 profile=high
chroma-format=4:2:0 bit-depth-luma=8 stream-format=obu-stream alignment=frame):
profile=high, the decoder accepts profile=main; rebuilding with av1dec (software fallback)
```

and a successful hardware attempt reports what it negotiated:

```
parse out: video/x-av1 profile=main chroma-format=4:2:0 bit-depth-luma=8 width=1920
height=1080 stream-format=obu-stream alignment=frame level=4.0 -- vaav1dec accepts it
```

The software decoder is therefore the last resort for a stream the hardware
decoder genuinely cannot take: another profile (VAAPI on RDNA4 advertises
`VAProfileAV1Profile0` while `vaav1dec`'s sink caps are `profile=main` only, so a
4:4:4 `profile=high` stream is refused outright), or a driver that cannot be
initialised. It is never the normal path: on this hardware the AV1 the encoder
produces (`profile=main`, 4:2:0, 8- or 10-bit) matches `vaav1dec` exactly and
runs on the GPU.

The periodic decode line carries the decision, so an operator can tell the two
apart without reading startup logs:

```
in=800 chunks/2524 KiB demux=172 decoded=118 sink=118 level=0 KiB dec=vaav1dec fallback=0 state=PLAYING (SUCCESS) pending=VOID_PENDING stalled=0
in=469 chunks/2282 KiB demux=113 decoded=86  sink=86  level=0 KiB dec=av1dec    fallback=1 state=PLAYING (SUCCESS) pending=VOID_PENDING stalled=0
```

`dec=` is the decoder element in use; `fallback=1` means it is not the
hardware-preferred first entry. `decoded=` is the decoder output buffer count.

`decode_fallback_test` (ctest) asserts the branch caps derived for each AV1
decoder, generates the encoder-representative fixture (NV12 1080p into the
hardware AV1 encoder, `av1parse`, frame-aligned caps, `mpegtsmux alignment=7
enable-custom-mappings=true`), reads that fixture's real caps back through
`tsdemux ! av1parse` and asserts `profile=main`, 4:2:0, 8-bit, 1080p, then
asserts the hardware decoder ran it (`dec=vaav1dec fallback=0`, no failed
attempt). A `profile=high` 4:4:4 fixture checks the refusal is named and that
frames still flow on `av1dec`; a forced first-attempt failure and an undecodable
stream check the rebuild path and that the chain is finite.
