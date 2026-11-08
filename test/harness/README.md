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
| `transcode-video-only` | NV12 frames, no audio at all | publishes once the audio-pad probe retires the capless pad (unlink + EOS) |
| `copy-av` | H.264+AAC MPEG-TS | publishes |
| `copy-av-audio-first` | H.264+AAC MPEG-TS muxed so tsdemux exposes its **audio** pad first (the same clip, audio branch declared first) | publishes; `demux pad order` must start with the audio pad |
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
| `copy-av-audio-first` | PASS | same numbers as `copy-av`, with `demux pad order audio_0_0041 (audio/mpeg), video_0_0042 (video/x-h264)` |
| `copy-video-only` | PASS (expected stall) | 53 video buffers at the muxer, 0 emitted, server +288 B (handshake only) |
| `transcode-video-only` | PASS | 109 video buffers at the muxer, 132 emitted, server +1 190 073 B, no audio buffer at the muxer |

The H.264 fixtures state `format=NV12` and nothing wider: an unpinned
`videotestsrc ! x264enc` negotiates High 4:4:4 10-bit on this box, which is not
what the encoder sends (4:2:0 8-bit) and not what any destination decodes.

### The capless audio pad

flvmux keeps a linked request pad in its collect-pad aggregate even after its
peer is unlinked. With no caps on that pad the muxer never completes
negotiation and emits nothing, so video backs up through the queue into
`vah264enc` and then into the video appsrc, and the stall watchdog rebuilds the
attempt after `k_stall_timeout`. Isolated on the same flvmux/vah264enc chain
with a starved audio appsrc: doing nothing yields 0 muxer buffers over the next
4 s, unlinking the peer also yields 0, and unlinking then pushing EOS on the
muxer's audio sink pad yields 273.

`output::retire_empty_audio_pad` therefore sends `gst_event_new_eos()` on the
muxer's audio sink pad after the unlink, logs whether the EOS was accepted, and
only then reports the pad dropped. It runs from the feed loop and from the
backpressure wait (a muxer that is not collecting is what backfills the
appsrc), and its deadline is 500 ms after the first video frame reaches the
encoder.

## Which code was measured

A verdict is about the commit the binary was linked against. The binary prints it
(`build commit   <sha>`), and the script refuses to run at all when any file under
`source/`, `test/source/` or the two `CMakeLists.txt` is newer than the executable
— it exits 2 with both timestamps instead. A stale binary produces numbers that
look exactly like a live regression (the pre-fix sources stall the muxer after
three buffers, server delta 576 B), and nothing in its report says which code it
measured.

## Sustained, not just started

The 32 KiB floor proves media left the box, not that it kept leaving it: the
harness stops feeding as soon as the floor is met, so a publisher that emits a
burst and then goes quiet would look identical to a healthy one. For
`transcode-video-only` the sustained case was measured separately by running the
binary for 30 s with `--duration 30` against a stat endpoint pinned at
`<bytes_in>0</bytes_in>` (so the early exit never triggers): the sink's counter
climbed 1.2 MB per 4 s without a gap for the whole run — 873 video buffers at the
muxer, 907 emitted, ~2.4 Mbit/s against a 2 Mbit/s target.

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
| 1280x720, 300 frames | 298/298, `vaav1dec`, PLAYING | 0 | +2.38 MB | H.264 1280x720, 92 frames / 2.99 s (30.8 fps) |
| 640x360, 300 frames | 296/296, `vaav1dec`, PLAYING | 0 | +2.10 MB | H.264 640x360, 90 frames / 3.01 s (30.0 fps) |

Both run twice, exit 0 both times. The first published frame leaves the muxer
~0.5 s after the first decoded frame reaches the encoder (logged as
`first published frame: N ms ...`); before that, the audio-pad probe is the
only thing the output waits on.

`FIXTURE_WIDTH` / `FIXTURE_HEIGHT` (default 1280x720) size the generated
fixture, so the same run can be repeated at another resolution with
`FIXTURE=… FIXTURE_WIDTH=640 FIXTURE_HEIGHT=360`.

### Transcode throughput

The feed reports its own accounting every 2 s while a transcode attempt runs:

    transcode feed: iters=75 consumed=71 pushed=71 blocked=0 (0 ms/iter,
    0 ms total) level=1350/10800 KiB peak=1350 KiB read=28.69 ms push=0.00 ms
    enc_in=70 mux_out=75 ring_drops=0 in 2.0 s

`level/cap` is the video `appsrc`'s current level against its cap, `blocked`
counts passes that found it full, `enc_in`/`mux_out` are what reached the
encoder and what the muxer emitted, and `ring_drops` is the frame ring's
cumulative drop count. A pinned level with `blocked` ≈ 100 % of the passes and
`ring_drops` climbing is the output starving the encoder, not the encoder
starving the output.

Two things keep 720p at ingest rate:

- The appsrc cap is a frame count (`k_appsrc_frame_span = 8`), not a fixed byte
  count, and a full appsrc is waited on through a probe on its src pad instead
  of a fixed per-pass sleep.
- The transcode `rtmp2sink` runs `sync=false`. Its input is a live
  drop-oldest ring, so pacing the encoder from the output clock turns every
  transient downstream delay into a permanent deficit: the frames needed to
  catch up are the ones the ring already dropped. The published timeline keeps
  the source PTS; only the sink's own clock sync is gone.
