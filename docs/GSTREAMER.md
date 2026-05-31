# GSTREAMER.md — verified receiver pipeline templates

Date: 2026-05-30
Box: GStreamer 1.28.3, gcc 16. "Verified locally" = tested on this box. "Deployment-only" = element absent here but
the pipeline string must still be generatable. All tests referenced (TEST A–H) are from the GStreamer research pass.

The receiver builds **one** `gst_parse_launch` pipeline per session: input chain → video (copy|reencode) ending in
`tee name=vtee` → audio (copy|reencode) ending in `tee name=atee` → one output-template fragment per destination.
One pipeline, one bus, one state machine, unified teardown (matches encoder `encode.cpp` lifecycle).

---

## 0. Element availability on this box

PRESENT (testable locally): `appsrc`, `queue`, `queue2`, `tsparse`, `tsdemux`, `tee`, `h264parse`, `h265parse`,
`av1parse`, `aacparse`, `avdec_h264`, `avdec_h265`, `av1dec`, `avdec_aac`, `decodebin3`, `parsebin`, `x264enc`,
`x265enc`, `avenc_aac`, `videoconvert`, `videoscale`, `videoconvertscale`, `audioconvert`, `audioresample`,
`flvmux`, `rtmp2sink`, `rtmpsink`, `mpegtsmux`, `rtpmp2tpay`, `srtsink`, `ristsink`.

ABSENT here — **deployment-box-only** (strings must still be generatable, cannot be tested locally): `nvh264enc`,
`nvh265enc`, `nvav1enc`, `nvh264dec`, `nvh265dec`, `nvav1dec`, `amfh264enc`, `amfh265enc`, `amfav1enc`, `qsvh264enc`,
`qsvh265enc`, `qsvav1enc` (and `qsv*dec`), `cudascale`, `cudaconvertscale`, `cudaupload`/`cudadownload`,
**and `rav1enc`** (so software AV1 *encode* is also deployment-only; only AV1 *decode* `av1dec` is present).

**Implication.** Fully testable locally: all copy paths, software h264/h265 reencode, and all three output protocols
(rtmp/srt/rist). The receiver MUST build encoder/decoder fragments from templates without requiring the element to
exist at build time, and at start-up validate the requested encoder/decoder family via registry lookup, returning a
control-plane error (`encoder_unavailable`, see CONTRACT §4) rather than emitting an unparseable string.

---

## 1. Input chain (RISTNetReceiver appsrc → MPEG-TS demux)

The RIST wrapper hands raw 188-byte TS (exactly what the encoder's `mpegtsmux alignment=7 + appsink` produced).
Use the wire-compatible idiom from `ndi-rist-server/main copy.cpp` (verified):

```
appsrc name=videosrc emit-signals=false block=true is-live=true do-timestamp=true format=time
       stream-type=0 max-bytes=0
  ! queue2
  ! tsparse set-timestamps=true alignment=7
  ! tsdemux name=demux
```

Set caps in C **after** `gst_parse_launch`, **before** PLAYING, on the `videosrc` element:
```c
GstCaps* caps = gst_caps_new_simple("video/mpegts",
                                    "systemstream", G_TYPE_BOOLEAN, TRUE,
                                    "packetsize",   G_TYPE_INT,     188,
                                    NULL);
gst_app_src_set_caps(GST_APP_SRC(videosrc), caps);
gst_caps_unref(caps);
```
Do **not** use `application/x-rtp,media=video,...MP2T` caps (that was the `main.cpp` udpsrc+rtpmp2tdepay path). Here the
RIST wrapper delivers raw TS.

Notes:
- `alignment=7` matches the encoder mux alignment (7×188 per buffer) for clean push boundaries.
- `block=true` gives flow back-pressure into the RIST `networkDataCallback` (desirable; do not hold any RIST mutex
  while blocked — you do not).
- `tsdemux` exposes **sometimes-pads** (dynamic). `gst_parse_launch` defers `demux.` linking until pads appear; this
  works (TEST E) but branches only negotiate once the first TS packets arrive. Do **not** assert demux pad linkage at
  parse time. Grab named elements (`vtee`, `videncoder`, sinks), never demux pads.
- The push recipe (memdup copy + `gst_app_src_push_buffer`, return 0 to keep the RIST connection) is in IMPL_PLAN.

---

## 2. Video copy (passthrough) chain — ends at `vtee`

Parser is chosen from `source.codec` (deterministic, no decodebin in copy mode):

```
demux. ! h264parse ! tee name=vtee       # source.codec == h264
demux. ! h265parse ! tee name=vtee       # source.codec == h265
demux. ! av1parse  ! tee name=vtee       # source.codec == av1
```
Optionally insert `queue silent=true` between `demux.` and the parse for buffering.

**DECISIVE RULE (TEST A failed; TEST D/E passed):** the single shared parser feeding the tee CANNOT serve byte-stream
(mpegts/rist) and avc (flv) simultaneously — the tee pushes identical buffers and caps negotiation deadlocks
(`not-negotiated`). Therefore the copy chain **ends at `vtee`**, and a **second, per-branch parser sits AFTER each tee
src pad** in the output template to negotiate the muxer-specific `stream-format`. Add `config-interval=1` (or `-1`) on
those post-tee parsers to re-insert SPS/PPS/VPS for mid-stream joiners.

---

## 3. Audio copy chain — ends at `atee`

Codec-independent (the encoder always emits AAC):
```
demux. ! aacparse ! tee name=atee
# or: demux. ! queue silent=true ! aacparse ! tee name=atee
```
As with video, **do not** lock `stream-format` before the tee: defer the raw-vs-adts conversion to a per-branch
`aacparse` after each `atee` src pad (flvmux needs `audio/mpeg,mpegversion=4,stream-format=raw`; mpegtsmux accepts
adts or raw — TEST G confirms aacparse negotiates raw for flvmux automatically). Use
`queue max-size-time=5000000000` on branches feeding muxers to absorb A/V interleave skew.

---

## 4. Video reencode matrix — DECODE → (optional upscale) → ENCODE → `vtee`

Fragments below mirror the encoder's `kEncoderTemplates[encoder][codec]` (`encode.cpp:197-253`): encoder element named
`videncoder`, `bitrate={BR}` (kbps), terminating in the output parse with `config-interval=1` (except av1). `{BR}` is
`video.bitrate_kbps`. Insert the upscale fragment (§6) between decode and encode when `upscale=true`.

### encoder = software (h264/h265 verified locally via TEST F; av1 encode deployment-only — rav1enc absent here)
| codec | fragment |
|-------|----------|
| h264 | `demux. ! h264parse ! avdec_h264 ! videoconvert ! x264enc name=videncoder bitrate={BR} speed-preset=fast tune=zerolatency key-int-max=120 ! video/x-h264,profile=high ! h264parse config-interval=1 ! tee name=vtee` |
| h265 | `demux. ! h265parse ! avdec_h265 ! videoconvert ! x265enc name=videncoder bitrate={BR} speed-preset=fast tune=zerolatency key-int-max=120 ! video/x-h265 ! h265parse config-interval=1 ! tee name=vtee` |
| av1  | `demux. ! av1parse ! av1dec ! videoconvert ! rav1enc name=videncoder bitrate={BR} speed-preset=8 tile-cols=2 tile-rows=2 ! video/x-av1 ! av1parse ! tee name=vtee` |

### encoder = nvenc (deployment-only)
Same-vendor CUDA dec→enc links directly; **no `videoconvert` between** `nvXdec`→`nvXenc` (that would force a
download/upload and break `(memory:CUDAMemory)` caps). Insert `cudaconvertscale`/`cudadownload` only to leave GPU or
rescale.
| codec | fragment |
|-------|----------|
| h264 | `demux. ! h264parse ! nvh264dec ! nvh264enc name=videncoder bitrate={BR} rc-mode=cbr-hq preset=low-latency-hq gop-size=120 ! h264parse config-interval=1 ! tee name=vtee` |
| h265 | `demux. ! h265parse ! nvh265dec ! nvh265enc name=videncoder bitrate={BR} rc-mode=cbr-hq preset=low-latency-hq gop-size=120 ! h265parse config-interval=1 ! tee name=vtee` |
| av1  | `demux. ! av1parse ! nvav1dec ! nvav1enc name=videncoder bitrate={BR} rc-mode=cbr preset=low-latency-hq gop-size=120 ! av1parse config-interval=1 ! tee name=vtee` |

### encoder = amd (deployment-only)
AMF has no Linux GStreamer hardware decoder element; decode with `avdec_*`/`av1dec` (or VA decoders if available),
then `amf*enc`. `videoconvert` IS needed (software decode → hw encode crosses memory).
| codec | fragment |
|-------|----------|
| h264 | `demux. ! h264parse ! avdec_h264 ! videoconvert ! amfh264enc name=videncoder bitrate={BR} rate-control=cbr usage=low-latency preset=quality pre-encode=true pa-hqmb-mode=auto ! video/x-h264,profile=high ! h264parse config-interval=1 ! tee name=vtee` |
| h265 | `demux. ! h265parse ! avdec_h265 ! videoconvert ! amfh265enc name=videncoder bitrate={BR} rate-control=cbr usage=low-latency preset=quality pre-encode=true pa-hqmb-mode=auto ! video/x-h265 ! h265parse config-interval=1 ! tee name=vtee` |
| av1  | `demux. ! av1parse ! av1dec ! videoconvert ! amfav1enc name=videncoder bitrate={BR} rate-control=cbr usage=low-latency preset=high-quality pre-encode=true pa-hqmb-mode=auto ! video/x-av1 ! av1parse config-interval=1 ! tee name=vtee` |

### encoder = qsv (deployment-only)
Same-vendor `qsv*dec`→`qsv*enc` trade `(memory:VAMemory)`/DMABuf and link directly. If the decode element is absent,
fall back to `avdec_* ! videoconvert ! qsv*enc`.
| codec | fragment |
|-------|----------|
| h264 | `demux. ! h264parse ! qsvh264dec ! qsvh264enc name=videncoder bitrate={BR} rate-control=cbr target-usage=1 gop-size=120 ! video/x-h264,profile=high ! h264parse config-interval=1 ! tee name=vtee` |
| h265 | `demux. ! h265parse ! qsvh265dec ! qsvh265enc name=videncoder bitrate={BR} rate-control=cbr target-usage=1 gop-size=120 ! video/x-h265 ! h265parse config-interval=1 ! tee name=vtee` |
| av1  | `demux. ! av1parse ! qsvav1dec ! qsvav1enc name=videncoder bitrate={BR} rate-control=cbr target-usage=1 gop-size=120 ! video/x-av1 ! av1parse config-interval=1 ! tee name=vtee` |

---

## 5. Audio reencode chain — ends at `atee` (always software, encoder-family-independent)

There is no hardware AAC encoder; always `avenc_aac` (TEST F: `avdec_aac` present, OK):
```
demux. ! aacparse ! avdec_aac ! audioconvert ! audioresample ! avenc_aac name=audencoder bitrate={BR_bps} ! tee name=atee
```
- `{BR_bps}` = `audio.bitrate_kbps * 1000` (avenc_aac wants bits/s); omit `bitrate=` to accept the default.
- `audioresample`+`audioconvert` are mandatory before `avenc_aac` (it needs standard sample rates/layout).
- Do **not** add an `aacparse` before `atee` that locks `stream-format` — defer per-branch `aacparse` to each output.

---

## 6. Upscale fragment (reencode-only; omitted when `upscale=false`)

Inserted between decode and encode. Copy mode cannot rescale.

- **Software (testable here):** `! videoscale ! video/x-raw,width={W},height={H} !`
  or single element `! videoconvertscale ! video/x-raw,width={W},height={H} !` (replaces videoconvert+videoscale).
- **CUDA (nvenc path, deployment-only):** keep frames on GPU —
  `nvh264dec ! cudaconvertscale ! video/x-raw(memory:CUDAMemory),width={W},height={H} ! nvh264enc ...`
  (`cudascale` also works where present).
- **QSV/VA:** `vapostproc ! video/x-raw(memory:VAMemory),width={W},height={H} !`.

`{W}`/`{H}` are `video.width`/`video.height` (even, bounds per CONTRACT §4).

---

## 7. Output templates (one per destination; `{N}` = unique index)

Every per-output element is uniquely named with `{N}` (`flvmux0`, `mpegtsmux1`, `ristsink2`, …) so
`gst_bin_get_by_name` / per-output stats work. `VPARSE` = `h264parse`/`h265parse`/`av1parse` matching the **output**
video codec.

### type = rtmp / rtmps  (verified TEST D)
```
vtee. ! queue ! h264parse config-interval=1 ! flvmux{N}.
atee. ! queue max-size-time=5000000000 ! aacparse ! flvmux{N}.
flvmux name=flvmux{N} streamable=true skip-backwards-streams=true
  ! queue
  ! rtmp2sink name=rtmpsink{N} location='{URL}/{KEY}'
```
- `flvmux` request pads are linked implicitly: a video-caps branch auto-requests the `video` pad, an audio-caps branch
  the `audio` pad (resolved by caps; this is why the per-branch parser must precede the mux).
- `rtmp2sink` (modern, async-connect) preferred; legacy `rtmpsink location='url/key' live=true` also works.
- **CONSTRAINT (§9): flvmux carries ONLY H.264(avc)+AAC(raw). No H.265, no AV1.** `rtmps://` URLs use the same
  template with the `rtmps` scheme.

### type = srt  (verified TEST B/H)
```
vtee. ! queue ! VPARSE config-interval=1 ! mpegtsmux{N}.
atee. ! queue max-size-time=5000000000 ! aacparse ! mpegtsmux{N}.
mpegtsmux name=mpegtsmux{N} alignment=7
  ! queue
  ! srtsink name=srtsink{N} uri='srt://{HOST}:{PORT}' mode=caller wait-for-connection=false streamid={STREAMID}
```
- `mpegtsmux` carries h264/h265/av1 + AAC (no codec restriction). `alignment=7` for network streaming.
- `mode=caller wait-for-connection=false` so an absent SRT consumer does not block startup.
- `{HOST}`/`{PORT}` parsed from `url`; `{STREAMID}` from `key_or_streamid`; `latency` from `params.latency_ms` may be
  appended to the uri (`?latency={ms}`).

### type = rist  (verified TEST H)
```
vtee. ! queue ! VPARSE config-interval=1 ! mpegtsmux{N}.
atee. ! queue max-size-time=5000000000 ! aacparse ! mpegtsmux{N}.
mpegtsmux name=mpegtsmux{N} alignment=7
  ! rtpmp2tpay
  ! ristsink name=ristsink{N} address={HOST} port={PORT} sender-buffer={BUF} cname={CNAME}
```
- Mirrors the encoder's own RIST input format: `mpegtsmux ! rtpmp2tpay` (produces `application/x-rtp media=video
  payload=33 MP2T`) `! ristsink` (Always sink pad, `application/x-rtp`).
- **`port` MUST be even** (RTCP uses `port+1`); an odd port silently misbehaves.
- `{BUF}` from `params.sender_buffer`; `{CNAME}` from `params.cname`. No codec restriction (mpegtsmux).

---

## 8. Pad-linking rules (validated on 1.28.3)

1. **One pipeline, one `gst_parse_launch` string** with tee + multiple muxers is the recommended approach (TEST A–F).
   Concatenate: input chain + video chain ending `tee name=vtee` + audio chain ending `tee name=atee` + per-output
   fragments.
2. **tee request src pads:** write `vtee. ! queue ! ...` once per consumer; each occurrence auto-requests a new
   `src_%u` pad. No explicit pad names. Set `tee allow-not-linked=true` only if a tee may momentarily have zero
   consumers.
3. **Muxer request sink pads:** the bare `name.` form (`mpegtsmux.` / `flvmux.`) auto-requests a sink pad and the muxer
   picks video-vs-audio pad template **by the caps of the incoming branch** — hence a per-branch parser must precede
   the muxer to fix caps (TEST D/E).
4. **QUEUE placement is mandatory:** a `queue` immediately after each tee src pad (decouples branches so one slow output
   cannot block the others) AND immediately before each network sink (decouples muxer from socket I/O). Use
   `max-size-time` on audio branches. For live restream resilience consider `queue leaky=downstream` on output branches
   so a stalled endpoint drops rather than back-pressuring the shared tee/appsrc (and thereby the RIST input).
5. **DECISIVE rule:** parsers go AFTER the tee, per-branch, never before (see §2/§3). One shared pre-tee parser causes
   `GST_FLOW_NOT_NEGOTIATED` the moment two outputs disagree on `stream-format`.

---

## 9. Codec/container constraints

**flvmux / all RTMP output:** verified from sink-pad caps on this box — video pad lists `video/x-h264` ONLY (no
`video/x-h265`, no `video/x-av1`); audio `audio/mpeg, mpegversion=4|2, stream-format=raw` (AAC). Vanilla flvmux 1.28
has no HEVC/AV1 mapping (enhanced-RTMP unsupported upstream).

Consequences (enforced in control logic — CONTRACT §4):
- H.264 copy or H.264 reencode → RTMP: always fine.
- H.265/AV1 (copied or reencoded) + RTMP output: **INVALID**. v1 rejects with `rtmp_codec_unsupported`. (v2 option:
  force an H.264 reencode sub-branch off the demux feeding only flvmux, while mpegts/SRT/RIST outputs still copy the
  original codec — a second `vtee` fed by an H.264 reencoder.)
- AAC into flvmux: `aacparse` converts adts→raw automatically (TEST G).

**mpegtsmux (SRT and RIST):** sink caps include `video/x-h264`, `video/x-h265`, `video/x-av1` + AAC — no codec
restriction.

---

## 10. Pitfalls (ranked)

1. **TEE/PARSER ORDERING** — never share one parser across outputs; put `h264parse`/`aacparse` AFTER each tee src pad.
   The single most likely bug (TEST A failed; TEST D/E passed).
2. **RTMP CODEC** — flvmux cannot carry H.265/AV1; detect codec+protocol mismatch in control logic, do not emit an
   unlinkable pipeline.
3. **appsrc CAPS in C** — set `video/mpegts,systemstream=true,packetsize=188` via `gst_app_src_set_caps` after parse,
   before PLAYING. Use `emit-signals=false` + the C push API (not raw caps in the launch string).
4. **tsdemux sometimes-pads** — deferred linking; do not assert at parse time.
5. **QUEUES** — missing post-tee queues serialize branches; one blocked sink stalls the whole pipeline and
   back-pressures the RIST appsrc, dropping the incoming stream. Add `queue` (consider `leaky=downstream`) on every
   output branch.
6. **ristsink even port** — odd ports misbehave; `srtsink`/`ristsink` use `wait-for-connection=false`/non-blocking so
   an absent downstream does not freeze startup.
7. **HW dec→enc memory** — same-vendor pairs keep frames in GPU/VA memory and link directly; do NOT insert plain
   `videoconvert` between them. Use `cudaconvertscale`/`cudadownload` only to leave GPU or rescale. Mixed (software
   decode → hw encode) DOES need `videoconvert`.
8. **avenc_aac negotiation** — always precede with `audioresample ! audioconvert`.
9. **config-interval=1 (or -1)** on the OUTPUT video parser re-inserts SPS/PPS/VPS for late-joining consumers (CDNs,
   SRT pulls).
10. **Deployment-box element validation** — nvenc/amf/qsv and rav1enc are absent here; validate the requested
    encoder/decoder via registry lookup at start and return `encoder_unavailable` (CONTRACT §4), never emit an
    unparseable string.
11. **flvmux for live** — set `streamable=true` and consider `skip-backwards-streams=true`.
12. **One pipeline / one bus** — keep all outputs in one `gst_parse_launch`; teardown is set-NULL + unref in reverse.
    Runtime add/remove of an output requires dynamic tee pad block/request/release/sync — the simple path is
    rebuild-and-restart on output-set changes.
