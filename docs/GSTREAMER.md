# GSTREAMER.md — receiver decode→raw-handoff pipeline

Date: 2026-06-05
Deployment box: stream2 — Ubuntu 26.04, kernel pinned 6.8.0-124, NVIDIA A40 vGPU (NVENC/NVDEC via `nvcodec`),
GStreamer 1.28.x. "Verified" below = exercised on stream2 (Phase 0 handoff spike, 2026-06-05).

> **Role (v1, 2026-06-05).** The receiver DECODES the incoming RIST/MPEG-TS stream and writes the **uncompressed**
> result to local loopback devices for a separate restreaming package (datarhei/restreamer) to encode + restream:
> raw video → **v4l2loopback** (`v4l2sink`), PCM audio → **ALSA snd-aloop** (`alsasink`). The receiver no longer
> encodes or muxes to RTMP/SRT/RIST. One `gst_parse_launch` pipeline, one bus, one teardown.

The receiver builds **one** pipeline per session: input chain (appsrc → tsdemux) → a video branch
(parse → decode → system-memory raw → `v4l2sink`) and an audio branch (parse → decode → PCM → `alsasink`).

---

## 0. Element availability

Required on the deployment box (validated at start via registry lookup; missing ⇒ control-plane `encoder_unavailable`,
CONTRACT §4):

- **Codec-independent (checked synchronously before detection):** `v4l2sink` (gst-plugins-good), `alsasink`
  (gst-plugins-base / `gstreamer1.0-alsa`), `videoconvert`, `audioconvert`, `audioresample`.
- **Input chain:** `appsrc`, `queue`/`queue2`, `tsparse`, `tsdemux`.
- **Video decoders (one of, by detected codec + `--no-hw-decode`):** NVDEC `nvh264dec`/`nvh265dec`/`nvav1dec` +
  `cudadownload` (preferred on stream2); VA `vah264dec`/… + `vapostproc`; QSV `qsv*dec` + `vapostproc`; software
  `avdec_h264`/`avdec_h265`/`av1dec`.
- **Audio parse/decode (by detected codec):** `aacparse`+`avdec_aac`, `opusparse`+`avdec_opus`,
  `ac3parse`+`avdec_ac3`/`avdec_eac3`, `mpegaudioparse`+`avdec_mp2float`.

Host modules (not GStreamer; see deployment notes): `v4l2loopback` (DKMS, out-of-tree — builds against the pinned
kernel) and `snd-aloop` (in-tree).

---

## 1. Input chain (RISTNetReceiver appsrc → MPEG-TS demux)

The RIST wrapper hands raw 188-byte TS (exactly what the encoder's `mpegtsmux alignment=7 + appsink` produced). Shared
by both the detection and real pipelines (`k_ts_source`):

```
appsrc name=videosrc is-live=true do-timestamp=true format=time stream-type=0
       max-bytes=4194304 block=true emit-signals=false
  ! queue2
  ! tsparse set-timestamps=true alignment=7
  ! tsdemux name=demux
```

Set caps in C **after** `gst_parse_launch`, **before** PLAYING, on `videosrc`:
```c
GstCaps* caps = gst_caps_new_simple("video/mpegts",
                                    "systemstream", G_TYPE_BOOLEAN, TRUE,
                                    "packetsize",   G_TYPE_INT,     188, NULL);
gst_app_src_set_caps(GST_APP_SRC(videosrc), caps);
```
Notes:
- `alignment=7` matches the encoder mux alignment (7×188 per buffer).
- `block=true` gives flow back-pressure into the RIST `networkDataCallback`.
- `tsdemux` exposes **sometimes-pads** (dynamic). `gst_parse_launch` defers `demux.` linking until pads appear; do
  **not** assert demux pad linkage at parse time. The push recipe (memdup copy + `gst_app_src_push_buffer`, always
  return 0 to keep the RIST connection) is in `restream::push_buffer`.

---

## 2. Codec detection (phase 1)

Before committing to a decode pipeline, a throwaway pipeline (`k_ts_source` + three `fakesink`s) is fed by the same
`push_buffer` path; `tsdemux` pad caps reveal the real video **and** audio codecs (`record_caps`). Detection ends on
`no-more-pads`, both codecs found, or a 5 s timeout (falls back to the declared `source.codec` for video, AAC for
audio). The detected codecs select the parser + decoder for phase 2 and are published to `GET /status`
(`restream::finish_detection_and_launch`).

---

## 3. Video branch — decode → system-memory raw → v4l2sink

The decoder is chosen at launch from the detected codec and `--no-hw-decode` (`choose_video_decoder`): probe the
registry for NVDEC, then VA, then QSV; else the libav software decoder. GPU output is brought to **system memory** for
the v4l2 device (`raw_download`), since a separate-process FFmpeg consumer cannot share GPU surfaces — the realistic
"raw" path is decoded CPU frames at the configured `--pixel-format` (default **NV12**, NVDEC-native — no conversion).

```
# NVDEC (preferred on stream2)
demux. ! video/x-h264 ! queue ! h264parse ! nvh264dec ! cudadownload ! videoconvert ! video/x-raw,format=NV12 !
        queue leaky=downstream max-size-buffers=4 ! v4l2sink name=vsink device='/dev/video10' sync=true
# VA-API
demux. ! video/x-h264 ! queue ! h264parse ! vah264dec ! vapostproc ! video/x-raw,format=NV12 ! ... ! v4l2sink ...
# software
demux. ! video/x-h264 ! queue ! h264parse ! avdec_h264 ! videoconvert ! video/x-raw,format=NV12 ! ... ! v4l2sink ...
```
- The `video/x-h26x` caps after `demux.` select the dynamic video pad (parser matches the **detected** codec).
- `cudadownload ! videoconvert` (CUDA) / `vapostproc` (VA) / `videoconvert` (sw) all yield plain
  `video/x-raw,format=NV12` in system memory; `videoconvert` is a no-op for already-NV12 8-bit content.
- `v4l2sink sync=true` paces to the pipeline clock (real-time); v4l2loopback drops old frames if no reader, so the
  sink does not stall the pipeline.

## 4. Audio branch — decode → PCM → alsasink

Audio is always decoded to PCM (`audio_decode_fragment`); `audioconvert`/`audioresample` normalise any source
layout/rate to the device's fixed format:
```
demux. ! audio/mpeg ! queue ! aacparse ! avdec_aac ! audioconvert ! audioresample !
        audio/x-raw,format=S16LE,channels=2,rate=48000 !
        queue leaky=downstream max-size-time=200000000 ! alsasink name=asink device='hw:Loopback,0,0' sync=true
```
(`audio/x-opus`→`opusparse`+`avdec_opus`, `audio/x-ac3`→`ac3parse`+`avdec_ac3`, `audio/x-eac3`→`ac3parse`+
`avdec_eac3`, `audio/mpeg` mpegversion 1→`mpegaudioparse`+`avdec_mp2float`.)

---

## 5. Full pipeline (assembled by `build_pipeline_string`)

```
<k_ts_source>
demux. ! <video-caps> ! queue ! <vparse> ! <vdecoder> ! <download> ! videoconvert ! video/x-raw,format=NV12 !
        queue leaky=downstream max-size-buffers=4 ! v4l2sink name=vsink device='<v4l2-device>' sync=true
demux. ! <audio-caps> ! queue ! <aparse> ! <adecoder> ! audioconvert ! audioresample !
        audio/x-raw,format=S16LE,channels=2,rate=48000 !
        queue leaky=downstream max-size-time=200000000 ! alsasink name=asink device='<audio-device>' sync=true
```
Single consumer per stream — **no tee**. Device paths are single-quoted and validated (`is_pipeline_safe`); the pixel
format is a validated bare token interpolated into the caps.

---

## 6. Pitfalls (ranked)

1. **Module load order / exclusive_caps.** Load `v4l2loopback exclusive_caps=1` so the device presents as a *capture*
   device to the FFmpeg reader once `v4l2sink` (output) has opened it; load `snd-aloop` (in-tree). The reader uses the
   **paired** ALSA subdevice (write `hw:Loopback,0,0` → read `hw:Loopback,1,0`).
2. **GPU→CPU is mandatory across processes.** Keep no `(memory:CUDAMemory)`/`(memory:VAMemory)` caps on the way to the
   sink — `cudadownload`/`vapostproc` must land frames in system memory, or `v4l2sink` cannot consume them.
3. **Bounded leaky queues before each sink.** Raw video is huge; cap the video queue by buffers
   (`max-size-buffers=4 leaky=downstream`) and audio by time so a stalled device drops frames instead of
   back-pressuring the shared demux/appsrc (and thereby the RIST ingest). v4l2loopback also drops on its own.
4. **tsdemux sometimes-pads** — deferred linking; do not assert at parse time. Grab named elements (`vsink`, `asink`),
   never demux pads.
5. **appsrc caps in C** — set `video/mpegts,systemstream=true,packetsize=188` via `gst_app_src_set_caps` after parse,
   before PLAYING. Use `emit-signals=false` + the C push API.
6. **A/V sync across two devices.** Both sinks `sync=true` to the single pipeline clock; the downstream FFmpeg consumer
   resynchronises the two device inputs on ingest. Verify glass-to-glass alignment on a sustained run.
7. **Compiler/kernel pin.** The deployment box's NVDEC/`nvcodec` + `v4l2loopback` depend on the pinned 6.8 kernel and
   the matched NVIDIA driver — do not boot a different kernel (see the deployment notes / project memory).
8. **Registry validation** — validate the codec-independent sink elements synchronously at `/start`, and the
   codec-specific decoder once detection has run; never emit an unparseable pipeline (return `encoder_unavailable`).
