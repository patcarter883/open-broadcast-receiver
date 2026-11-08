# Local RTMP publish harness

Two harnesses, both publishing to a throwaway local RTMP server:

| harness | what it proves |
| --- | --- |
| `rtmp_publish_harness.sh` | the `output` object publishes, driven in-process with fixtures (no RIST, no decode) |
| `rist_av1_publish_e2e.sh` | the whole product path: AV1-in-MPEG-TS over RIST → decode → frame ring → transcode → RTMP |

## `rist_av1_publish_e2e.sh` — the end-to-end proof

    test/harness/rist_av1_publish_e2e.sh [seconds]

It builds a synthetic AV1+AAC MPEG-TS with the encoder's own muxing
(`mpegtsmux alignment=7 enable-custom-mappings=true`, AV1 carried as stream type
`0x06` with registration `AV1G`), checks that `tsdemux` exposes `video/x-av1` on
it, starts a local `mediamtx`, starts the receiver binary from this tree, and
POSTs `/start` with an AV1 source and an RTMP output that transcodes to H.264.
`test/tools/rist_feed` then sends the TS over a real RIST socket (ADVANCED
profile, 7×188-byte chunks), so the RIST listener, the decode stage, the frame
ring and the output are all the product's own code.

Sample run (30 s):

```
t    bytes_sent     state        recon  dropped   recorded_bytes
6s   34080376       running      0      0         0
12s  169670900      running      0      105       808516
24s  313669832      running      0      405       2266804
36s  441003520      running      0      593       3830022

bytes_sent: first=34080376 last=441003520 delta=406923144
recorded:   3830022 bytes   publishing_logged=1   reconnects=0
last segment: 20261108-191557.mp4 2.465000s h264,1280,720 (aac,48000,2)
PASS
```

PASS requires all of: `bytes_sent` grew, the output never reconnected, mediamtx
logged `is publishing to path 'live/e2e'`, and mediamtx wrote at least 256 KiB
of media. The last word is the sink's own file, which is probed for an H.264
video track at the ingest resolution and an AAC 48 kHz stereo track.

Two configuration facts the harness depends on:

- **The receiver refuses loopback destinations.** `classify()` treats `127.0.0.0/8`
  as `loopback` regardless of `--egress-allow-private`, so the harness publishes
  to this host's LAN address with `--egress-allow-private` — the same flag an
  operator uses to restream into a LAN. Nothing leaves the box.
- **The RTMP URL is the app, `key_or_streamid` is the stream.** `url` +
  `key_or_streamid` become rtmp2sink's `application` and `stream`; putting the
  stream name in the URL publishes to an application nothing serves, and the
  connection then sits idle until the sink's stall watchdog rebuilds it.

`mediamtx` is used because it is already installed on the build boxes; its
record directory (`build/e2e/rec/<app>/<stream>/`) is the measured artifact. It
needs no container and no config beyond the one the harness writes.

### Defects this harness found

Both made a healthy-looking pipeline never publish:

- **The audio branch's demux was chained to whichever pad appeared first.** The
  transcode template wrote `tsdemux name=ad queue name=aq ! aacparse …`. For an
  AV1+AAC multiplex the demuxer exposes the video pad first, so the video PID
  occupied the audio chain (`aacparse` then refused it) and the audio pad had no
  peer at all — `tsdemux` returned `NOT_LINKED` and flvmux never emitted. Fixed by
  leaving `tsdemux` unchained and linking every pad explicitly in
  `demux_pad_added`: audio to `aq`, everything else to a `funnel ! fakesink
  async=false` drop branch.
- **Decoded frames with no PTS were published at 0.** `vaav1dec` leaves PTS unset
  for a frame whose PES carried none (and emits no DTS). Publishing those as 0
  put a frame at the head of the timeline mid-stream, so flvmux/rtmp2sink held
  the frames after it: the publish ran in ~5 s bursts and the stall watchdog
  rebuilt the output every cycle. `pts_synth` now gives the destination a
  monotonic timeline, with a unit test in `test/source/pts_synth_test.cpp`.

## `rtmp_publish_harness.sh` — the output object in-process

Proves the receiver's RTMP output path actually publishes with no capture box
and no RIST chain. The ingest side is replaced by fixtures GStreamer generates
locally; the destination is a local nginx-rtmp sink.

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
| `transcode-video-only` | NV12 frames, no audio at all | publishes once the 4 s probe retires the capless audio pad (unlink + EOS) |
| `copy-av` | H.264+AAC MPEG-TS | publishes |
| `copy-video-only` | H.264-only MPEG-TS | does **not** publish (the copy template has no timeout unlink, so flvmux waits for audio forever) |

`copy-video-only` is a test of a known gap, not a flake: it passes when the
server receives nothing and the muxer's source pad stays at zero.

## PASS rule

A mode passes when the server's `bytes_in` grew by at least 32 KiB after at least
2 s of feeding, matching the mode's expectation. The floor exists because
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
| `transcode-av` | PASS | 61 frames pushed, 61 video + 89 audio buffers at the muxer, 136 buffers emitted, server +596 246 B |
| `copy-av` | PASS | 69 video + 109 audio buffers at the muxer, 90 emitted, server +281 319 B |
| `copy-video-only` | PASS (expected stall) | 52 video buffers at the muxer, 0 emitted, server +288 B (handshake only) |
| `transcode-video-only` | PASS | 137 frames pushed, 110 video buffers at the muxer, 133 emitted, server +1 202 044 B |

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
