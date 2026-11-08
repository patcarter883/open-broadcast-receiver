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
| `copy-av` | H.264+AAC MPEG-TS, video stream first in the PMT | publishes |
| `copy-audio-first` | the same clip with the audio stream first in the PMT, so `tsdemux` announces the audio pad before the video pad | publishes, audio as AAC on the muxer's audio pad and H.264 as AVC on its video pad |
| `copy-non-aac` | H.264 with AC-3 audio: an audio pad no branch can carry | publishes video-only; the AC-3 pad is sunk by the drop funnel, `audio_dropped: true` |
| `copy-video-only` | H.264-only MPEG-TS | publishes video-only, after the 4 s probe retires the silent audio pad |

Every mode must publish; the server's counter decides. `copy-audio-first` is the
one that would catch a pad-order regression: the branch a demuxer pad reaches is
decided from that pad's caps, so arrival order cannot put audio into `h264parse`.

## PASS rule

A mode passes when the server's `bytes_in` grew by at least 32 KiB after at least
2 s of feeding, the caps that reached the muxer's video pad are H.264 4:2:0
8-bit, and the encoder's sink pad is `video/x-raw, format=NV12`. Those caps are
read from the running pipeline, not from the template, so a negotiation that
stopped pinning the format fails the mode instead of silently publishing High
4:4:4 10-bit. The floor exists because
nginx-rtmp counts the RTMP handshake and the connect/publish commands (~3 KB): a
publisher that connects and then sends nothing would otherwise look healthy —
exactly the shape of the live-rig failure. The video-only mode additionally
requires that no audio buffer reached the muxer, and the audio modes that at
least one did. The copy modes additionally require the muxer's video pad caps to
be `video/x-h264, stream-format=avc` and its audio pad caps to be
`audio/mpeg, ..., stream-format=raw`: only `h264parse` and `aacparse` produce
those, so the caps name the branch each stream reached.

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
| `copy-av` | PASS | `fixture pad order video_0_0041 audio_0_0042`; 69 video + 109 audio buffers at the muxer, 90 emitted, server +283 023 B |
| `copy-audio-first` | PASS | `fixture pad order audio_0_0041 video_0_0042` (audio announced first); 69 video + 109 audio buffers at the muxer, 90 emitted, server +283 029 B |
| `copy-non-aac` | PASS | `fixture pad order video_0_0041 audio_0_0042`; AC-3 pad sunk, 112 video buffers at the muxer, 87 emitted, server +678 726 B |
| `copy-video-only` | PASS | `fixture pad order video_0_0041`; 112 video buffers at the muxer, 87 emitted, no audio buffer, server +678 726 B |
| `transcode-video-only` | PASS | 109 video buffers at the muxer, 132 emitted, server +1 190 073 B, no audio buffer at the muxer |

The H.264 fixtures state `format=NV12` and nothing wider: an unpinned
`videotestsrc ! x264enc` negotiates High 4:4:4 10-bit on this box, which is not
what the encoder sends (4:2:0 8-bit) and not what any destination decodes.

### The capless audio pad

flvmux keeps a linked request pad in its collect-pad aggregate even after its
peer is unlinked. With no caps on that pad the muxer never completes
negotiation and emits nothing, so video backs up into the 4 MB appsrc (transcode:
through `vah264enc` first), and a copy-only output simply never prerolls.
Isolated on the same flvmux/vah264enc chain with a starved audio appsrc: doing
nothing yields 0 muxer buffers over the next 4 s, unlinking the peer also yields
0, and unlinking then pushing EOS on the muxer's audio sink pad yields 273.

`output::probe_muxer_audio`, run 4 s into every RTMP attempt from both feed paths
(copy and transcode), therefore sends `gst_event_new_eos()` on the muxer's audio
sink pad after the unlink, logs whether the EOS was accepted, and only then
reports the pad dropped.

## Notes

- No `decodebin`, no `ristsink`: the fixtures come from `videotestsrc` /
  `audiotestsrc` / `x264enc` / `avenc_aac`, and the output is the production RTMP
  template.
- `mpegtsmux` needs no `enable-custom-mappings` here. That flag exists for AV1,
  which has no standard PMT stream type; AAC and H.264 do.
- The harness publishes. Run it only against a sink you started for it; the
  script removes its own container by name on exit and touches nothing else.
- Ports are overridable: `RTMP_PORT=21935 STAT_PORT=28080 ...`.
