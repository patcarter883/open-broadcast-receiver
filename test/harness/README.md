# Local RTMP publish harness

Proves the receiver's RTMP output path actually publishes — no capture box, no
RIST chain, no platform. The ingest side is replaced by fixtures GStreamer
generates locally; the destination is a local nginx-rtmp sink.

    test/harness/rtmp_publish_harness.sh

The point is the verdict rule: **the server's own byte count decides**. The
receiver's `bytes_sent` counts pushes into the appsrc and has read healthy on a
rig while the connection sat idle, so the harness never trusts it. Likewise the
muxer's sink pads are probed rather than the appsrc's caps, because an appsrc can
be negotiated and accepting buffers while everything downstream of it is stalled.

## What it drives

`output` from `source/fanout/output.cpp` — the same `start()` /
`run_once()` / `feed_transcode()` code the binary runs. The harness only supplies
the two rings' contents and reads counters; it never builds a pipeline of its own.
`output::pipeline()` returns a reference to the current attempt's pipeline so the
harness can install pad probes and read element state.

## The sink

Image: `alfg/nginx-rtmp:latest` (nginx 1.23.1, nginx-rtmp 1.1.4).

    docker run -d --rm --name obr-rtmp-sink-$$ \
      -p 11935:1935 -p 18080:8080 \
      -v "$PWD/docker/rtmp-sink/nginx.conf:/etc/nginx/harness.conf:ro" \
      alfg/nginx-rtmp:latest nginx -c /etc/nginx/harness.conf -g 'daemon off;'

`docker/rtmp-sink/nginx.conf` defines one ingest application (`live`) and
`/stat`. The config is mounted at `/etc/nginx/harness.conf`, not at
`/etc/nginx/nginx.conf`: the image's entrypoint rewrites that path, so a
read-only mount there fails and a writable one is overwritten.

`/stat` reports the server-wide `<bytes_in>` first, before any `<stream>`
section. That counter survives the publisher disconnecting (per-stream sections
do not), and it is what the harness reads.

## Modes

| mode | ring contents | expectation |
| --- | --- | --- |
| `transcode-av` | NV12 frames + AAC-in-MPEG-TS | publishes |
| `transcode-av1` | AV1-in-MPEG-TS on the ts_ring, decoded by the receiver's own decode stage into the frame ring (`--fixture`, `FIXTURE=`) | publishes, with `vaav1dec` having produced frames |
| `transcode-video-only` | NV12 frames, no audio at all | publishes once the 4 s probe retires the capless audio pad (unlink + EOS) |
| `copy-av` | H.264+AAC MPEG-TS | publishes |
| `copy-video-only` | H.264-only MPEG-TS | does **not** publish (the copy template has no timeout unlink, so flvmux waits for audio forever) |

`copy-video-only` is a test of a known gap, not a flake: it passes when the
server receives nothing and the muxer's source pad stays at zero.

## PASS rule

A mode passes when the server's `bytes_in` grew by at least 32 KiB after at least
2 s of feeding, matching the mode's expectation, and -- when it published -- when
the caps that reached the muxer's video pad are H.264 4:2:0 8-bit and the
encoder's sink pad is `video/x-raw, format=NV12`. Those caps are read from the
running pipeline, not from the template, so a negotiation that stopped pinning
the format fails the mode instead of silently publishing High 4:4:4 10-bit. The floor exists because
nginx-rtmp counts the RTMP handshake and the connect/publish commands (~3 KB): a
publisher that connects and then sends nothing would otherwise look healthy —
exactly the shape of the live-rig failure. The video-only mode additionally
requires that no audio buffer reached the muxer, and the audio modes that at
least one did.

Per-run report:

    frames pushed        frames written to the frame ring
    mux sink video       buffers/bytes that arrived at the muxer's video pad
    mux sink audio       same for the audio pad
    mux src emitted      buffers/bytes the muxer actually produced
    server bytes_in      the sink's counter, with baseline and delta
    output bytes_sent    the appsrc push counter, printed for comparison only

When a mode fails, the report also prints every element's state, each mux sink
pad's caps and link state, the `osrc`/`asrc` caps, all bus ERROR/WARNING/EOS
messages with their element and debug string, and the output's own log.

## Current results

Against `alfg/nginx-rtmp` on this box, with `vah264enc`:

| mode | result | numbers |
| --- | --- | --- |
| `transcode-av` | PASS | 61 frames pushed, 61 video + 90 audio buffers at the muxer, 138 emitted, server +608 689 B; encoder input `video/x-raw,format=NV12`; muxer video `profile=high, chroma-format=4:2:0, bit-depth-luma=8` |
| `copy-av` | PASS | 69 video + 109 audio buffers at the muxer, 90 emitted, server +283 032 B |
| `copy-video-only` | PASS (expected stall) | 53 video buffers at the muxer, 0 emitted, server +288 B (handshake only) |
| `transcode-video-only` | PASS | 109 video buffers at the muxer, 132 emitted, server +1 190 073 B, no audio buffer at the muxer |

The H.264 fixtures state `format=NV12` and nothing wider: an unpinned
`videotestsrc ! x264enc` negotiates High 4:4:4 10-bit on this box, which is not
what the encoder sends (4:2:0 8-bit) and not what any destination decodes.

### The capless audio pad

flvmux keeps a linked request pad in its collect-pad aggregate even after its
peer is unlinked. With no caps on that pad the muxer never completes
negotiation and emits nothing, so video backs up through the queue into
`vah264enc` and then into the 4 MB appsrc, and the stall watchdog rebuilds the
attempt every 4 s. Isolated on the same flvmux/vah264enc chain with a starved
audio appsrc: doing nothing yields 0 muxer buffers over the next 4 s, unlinking
the peer also yields 0, and unlinking then pushing EOS on the muxer's audio sink
pad yields 273.

`output::feed_transcode`'s 4 s probe therefore sends `gst_event_new_eos()` on
the muxer's audio sink pad after the unlink, logs whether the EOS was accepted,
and only then reports the pad dropped.

## Notes

- No `decodebin`, no `ristsink`: the fixtures come from `videotestsrc` /
  `audiotestsrc` / `x264enc` / `avenc_aac`, and the output is the production RTMP
  template.
- `mpegtsmux` needs no `enable-custom-mappings` here. That flag exists for AV1,
  which has no standard PMT stream type; AAC and H.264 do.
- The harness publishes. Run it only against a sink you started for it; the
  script removes its own container by name on exit and touches nothing else.
- Ports are overridable: `RTMP_PORT=21935 STAT_PORT=28080 ...`.

## Whole-binary e2e: RIST in, RTMP out

    test/harness/receiver_e2e_local.sh

`rtmp_publish_harness` drives `output` directly with synthetic ring contents.
This harness instead runs the **shipped binary, unmodified, from its own control
API**: a real AV1-in-MPEG-TS fixture goes over real RIST (`rist_feed`,
ADVANCED/SOURCE) into `--rist-port`, `POST /start` configures one transcode
output, and `/stats` is sampled once a second while the RTMP server's own
counters are read from MediaMTX's `/v3/paths/list`.

Nothing leaves the machine, and nothing needs the capture box.

### The fixture is checked before anything else runs

`test/tools/make-av1-ts-fixture.sh` muxes it with the encoder's exact settings
(`mpegtsmux alignment=7 enable-custom-mappings=true`, `av1parse` pinned to
`obu-stream,alignment=frame`). The harness then prints, and gates on:

- the PMT actually present in the file — `stream_type=0x06` with the `AV1G`
  registration descriptor and the `0x80` av1C record;
- `filesrc ! tsparse ! tsdemux` exposing a **`video/x-av1`** pad.

`ffprobe` is printed too, but it reports `codec_name=bin_data` for this
fixture: FFmpeg's MPEG-TS demuxer has no mapping for AV1 in a private
(0x06/AV1G) stream, so it *cannot* be the validity gate. A fixture that is
genuinely bin_data+AAC fails the PMT and tsdemux checks above, which is what
makes those two the real gate — the earlier, invalid `test_av1.ts` would have
failed them.

### Why the sink is not on 127.0.0.1

The receiver's egress policy refuses loopback unconditionally (`classify()` in
`source/lib/lib.cpp` returns `"loopback"` for 127.0.0.0/8 even with
`--egress-allow-private`). The harness therefore points at the box's own first
global IPv4 (`SINK_HOST=` to override) — still on this machine, still no
network — and runs MediaMTX bound to that address.

### PASS rule

All three, from data the receiver does not produce:

1. the decoder's staged counters show `decoded>0` and `state=PLAYING`;
2. the RTMP server's `bytesReceived` for the path grows by ≥ 32 KiB;
3. 3 s of the published stream are pulled **back out** of the RTMP server with
   `ffmpeg -c copy` and probe as H.264 video with frames in it.

`bytes_sent` is printed but never trusted: for a transcode output it counts the
NV12 frames pushed into the video `appsrc` (≈230 MB for 10 s of 720p), so it
grows healthily even when nothing reaches RTMP.

On failure the verdict names the first stage that stopped — RIST ingest, TS
ring, decode appsrc, tsdemux, decoder, or the output — with the counters that
prove it.

### Current results (this box, RX 9070, vaav1dec + vah264enc)

| fixture | decode | frames dropped | sink growth | pulled back |
| --- | --- | --- | --- | --- |
| 1280x720, 300 frames | 298/298, `vaav1dec`, PLAYING | 135–136 (~45 %) | +1.09–1.11 MB | H.264 1280x720, 40 frames / 3 s |
| 640x360, 300 frames | 297/297, `vaav1dec`, PLAYING | 0 | +2.33 MB | H.264 640x360, 91 frames / 3 s |

Both run twice, exit 0 both times. Publishing starts ~6 s after the first TS
chunk: the first 4 s are the audio-pad probe, which is what retires the
capless audio pad and lets `flvmux` emit. A feed shorter than ~6 s therefore
loses the race and looks like a stall.

### Known finding: 720p transcodes run at ~13 fps

At 1280x720 the frame ring drops ~45 % of decoded frames and the published
stream is ~13 fps (40 frames in 3 s, irregular PTS gaps), while the same run at
640x360 drops nothing and publishes a clean 30 fps. `vah264enc` alone does
284 fps at 720p on this box, so the encoder element is not the limit. The
shape of the numbers matches `feed_transcode()`'s byte-based throttle: the
video `appsrc` is capped at `k_appsrc_max_bytes = 4 MB`, a 720p NV12 frame is
1.38 MB (so ~2.9 frames fit), and each iteration that finds the level at or
over the cap sleeps 20 ms and then reads only one frame. Not fixed here — the
harness records it.
