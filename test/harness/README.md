# Local harnesses

Four runners, all publishing to a throwaway local server and all judging the run
by what the server received rather than by the receiver's own counters.

| runner | what it proves |
| --- | --- |
| `receiver_e2e_guard.sh` | ctest `receiver_e2e_guard`: the same path in ~9 s, asserting only the four things that must never regress (see below) |
| `receiver_e2e_local.sh` | the whole product path from an AV1-only TS: RIST → decode → frame ring → transcode → RTMP, with the sink's byte count and a stream read back out of the server |
| `rist_av1_publish_e2e.sh` | the same path from an AV1+AAC TS, judged by the media the sink recorded |
| `rtmp_publish_harness.sh` | the `output` object alone, driven in-process per mode (no RIST, no decode) |
| `standby_fanout_e2e.sh` | the standby slate fan-out: ONE destination connection across slate → live → slate, an encoder restart with a PTS reset, and a video-only ingest |

## `receiver_e2e_local.sh` — the end-to-end proof

    test/harness/receiver_e2e_local.sh

It generates an AV1-in-MPEG-TS fixture with the encoder's own muxing
(`mpegtsmux alignment=7 enable-custom-mappings=true`, AV1 carried as stream type
`0x06` with registration `AV1G`), proves `tsdemux` exposes `video/x-av1` on it,
starts a local `mediamtx`, starts this tree's receiver binary, and POSTs `/start`
with an AV1 RIST source and an RTMP output that transcodes to H.264.
`test/tools/rist_feed` sends the TS over a real RIST socket in 7×188-byte
datagrams, so the RIST listener, the decode stage, the frame ring and the output
are all the product's own code. Nothing leaves the machine and nothing needs the
capture box.

Sample run (300-frame fixture, 10 s of ingest):

```
t    rist_recv ts_in    out_state out_bytes_sent frames_dropped reconnects sink_path:ready  sink_bytes_received
4    500       947520   running   39654720       0              0          none:false       0
5    509       1705536  running   94263168       0              0          live/e2e:true    740250
8    478       3854564  running   252686564      0              0          live/e2e:true    1891712
12   0         6319432  running   418274632      0              0          live/e2e:true    3097955

[decoder] in=207 chunks/6171 KiB demux=433 decoded=298 sink=298 level=0 KiB dec=vaav1dec fallback=0 state=PLAYING (SUCCESS) pending=VOID_PENDING stalled=0
sink bytesReceived   740250 -> 3097955 (growth 2357705)
output state         running in 11 sample(s), reconnects 0
read back from sink  codec_name=h264 width=1280 height=720 nb_read_frames=92 (905347 B pulled)
VERDICT: PUBLISHES — 298 frames decoded by vaav1dec, sink received 2357705 B,
         and a decodable H.264 stream reads back out of the RTMP server
```

The verdict needs all of: a `running` output, `reconnects 0` (a publish that only
happened across rebuilt attempts is not a publish), the sink's own counter growing
by ≥ 32 KiB, and an H.264 stream at the ingest resolution read back out of the
server. `bytes_sent` is printed but never decides: for a transcode output it counts
NV12 frames pushed into the appsrc and grows even when nothing reaches RTMP.

Two configuration facts it depends on:

- **The receiver refuses loopback destinations.** `classify()` treats `127.0.0.0/8`
  as `loopback` regardless of `--egress-allow-private`, so the harness publishes
  to this host's LAN address with `--egress-allow-private` — the same flag an
  operator uses to restream into a LAN. Nothing leaves the box.
- **The RTMP URL is the app, `key_or_streamid` is the stream.** `url` +
  `key_or_streamid` become rtmp2sink's `application` and `stream`; putting the
  stream name in the URL publishes to an application nothing serves, and the
  connection then sits idle until the stall watchdog rebuilds it.

A reused fixture is fed at its own frame count: `make-av1-ts-fixture.sh` writes a
`<fixture>.meta` sidecar and the harness derives the feed rate from it, so a
fixture built with `FIXTURE_FRAMES=150` is not fed at half speed.

### What the e2e run pins down

- **Pad order cannot decide routing.** A `tsdemux` pad reaches the branch its caps
  belong to, linked in `output::demux_pad_added`; a chained template link binds
  whichever pad arrives first, and an AV1+AAC multiplex announces video first.
  Any pad no branch claims goes to the `ddrop` funnel, because an unlinked demux
  pad fails the pipeline with `NOT_LINKED`.
- **The published timeline is monotonic.** `vaav1dec` leaves PTS unset for a
  frame whose PES carried none and emits no DTS. Publishing those as 0 puts a
  frame at the head of the timeline mid-stream, and the destination's muxer then
  holds every frame after it. `pts_synth` gives the frames a monotonic timeline,
  covered by `test/source/pts_synth_test.cpp`.
- **Nothing time-based gates the first frame.** The muxer's audio pad is requested
  only when the source TS exposes an audio pad, so a video-only ingest publishes on
  its first encoded frame.

## `receiver_e2e_guard.sh` — the ctest gate

    ctest --test-dir build -R receiver_e2e_guard --output-on-failure

Registered from `test/CMakeLists.txt` in the full lane only, because it needs
`rist_feed`, GStreamer and the receiver binary. It is the regression gate for
AV1 ingest → hardware decode → RTMP publish, and it is self-contained: it
generates (or reuses) a 3 s 640x360 AV1-only TS fixture, picks four currently
free ports, starts its own MediaMTX, starts the receiver, POSTs `/start`, feeds
the fixture over RIST, and stops every process it started before returning —
including when CTest kills it on timeout.

It fails if, by the time the whole fixture has been fed:

- the decoder never logged counters, or its last line is not `state=PLAYING`;
- `decoded=0`;
- `dec=` is anything other than `vaav1dec` (a software fallback is a
  regression: this product requires hardware decode);
- the output never reports `running`, or the RTMP sink's `bytesReceived`
  stayed at 0.

It exits 77 (CTest SKIP) with the reason on a box with no usable
`/dev/dri/renderD*`, without the required elements/tools, or without a
non-loopback IPv4 to publish to. It never needs root and nothing leaves the
host.

```
[decoder] in=31 chunks/440 KiB demux=126 decoded=86 sink=86 level=0 KiB dec=vaav1dec fallback=0 state=PLAYING (SUCCESS) pending=VOID_PENDING stalled=0
[output local-rtmp] running=1 sink_bytesReceived=470601
GUARD OK: 86 frames decoded by vaav1dec, output running, sink received 470601 bytes
```

Two consecutive `ctest -R receiver_e2e_guard` runs take ~9 s each and leave no
`mediamtx`, `rist_feed` or receiver process behind.

## `rist_av1_publish_e2e.sh` — the same path with audio, judged by the recording

    test/harness/rist_av1_publish_e2e.sh [seconds]

The fixture is an AV1+AAC multiplex, so this run also exercises the audio branch
through a real RIST ingest: the muxer's audio pad is requested because the TS
announces one, and the recorded file carries both tracks. `mediamtx`'s record
directory (`build/e2e/rec/<app>/<stream>/`) is the measurement, probed for an
H.264 video track at the ingest resolution and an AAC 48 kHz stereo track. PASS
also requires the output never to have reconnected and the sink to have logged
`is publishing to path '<app>/<key>'`.

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
| `transcode-av1` | AV1-in-MPEG-TS on the ts_ring, decoded by the receiver's own decode stage into the frame ring (`--fixture`, `FIXTURE=`) | publishes, with `vaav1dec` having produced frames |
| `transcode-video-only` | NV12 frames, no audio at all | publishes on the first encoded frame: the muxer's audio pad is requested only when the source TS exposes an audio pad |
| `copy-av` | H.264+AAC MPEG-TS, video stream first in the PMT | publishes |
| `copy-audio-first` | the same clip with the audio stream first in the PMT, so `tsdemux` announces the audio pad before the video pad | publishes, audio as AAC on the muxer's audio pad and H.264 as AVC on its video pad |
| `copy-non-aac` | H.264 with AC-3 audio: an audio pad no branch can carry | publishes video-only; the AC-3 pad is sunk by the drop funnel, `audio_dropped: true` |
| `copy-video-only` | H.264-only MPEG-TS | publishes video-only: no audio pad is ever requested, so nothing waits on one |
| `reconnect` | NV12 frames, then the sink container is restarted under the running output (`--sink-restart-cmd`) | republishes: the rebuild opens at the connect bootstrap rate, reaches publish, and the encoder ramps back to the measured-link target |

Every mode must publish; the server's counter decides. `copy-audio-first` is the
one that would catch a pad-order regression: the branch a demuxer pad reaches is
decided from that pad's caps, so arrival order cannot put audio into `h264parse`.

`reconnect` is the only mode with a destination-side failure in it. It first
proves a publish, then feeds a link measurement (`--measured-kbps`, default 6000)
so the encoder runs at the derived target (5400 kbps at the default 2000 kbps
declared rate), then restarts the sink, which severs the publish. It passes only
if the rebuilt attempt applied the connect bootstrap rate rather than that
measured target, republished within the connect window, needed at most four
attempts, and ramped back to the measured target. A scenario where the bootstrap
and the measured target coincide fails by design: it would prove nothing.

The local sink publishes in well under a second, so this mode exercises the
drop-and-recover path but not a slow platform handshake. The connect-window
classification itself is covered by `pipeline_build_test`.

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
| `transcode-av` | PASS | 61 frames pushed, 61 video + 70 audio buffers at the muxer, 178 emitted, server +705 561 B; encoder input `video/x-raw,format=NV12`; muxer video `profile=high, chroma-format=4:2:0, bit-depth-luma=8` |
| `transcode-video-only` | PASS | 61 frames pushed, 61 video buffers at the muxer, 82 emitted, no audio buffer, server +681 789 B |
| `copy-av` | PASS | `demux pad order video_0_0041 (video/x-h264), audio_0_0042 (audio/mpeg)`; 69 video + 92 audio buffers at the muxer, 90 emitted, server +283 029 B |
| `copy-audio-first` | PASS | `demux pad order audio_0_0041 (audio/mpeg), video_0_0042 (video/x-h264)` (audio announced first); 69 video + 91 audio buffers at the muxer, 90 emitted, server +283 032 B |
| `copy-non-aac` | PASS | `demux pad order video_0_0041 (video/x-h264), audio_0_0042 (audio/x-ac3)`; the AC-3 pad is sunk, 69 video buffers at the muxer, 39 emitted, no audio buffer, server +264 020 B |
| `copy-video-only` | PASS | `demux pad order video_0_0041 (video/x-h264)`; 68 video buffers at the muxer, 39 emitted, no audio pad requested at all, server +264 023 B |
| `reconnect` | PASS | publish at 5400 kbps from a 6000 kbps measurement; sink restarted; rebuild applied the 2000 kbps connect bootstrap; republished, server +936 169 B after the drop; back at 5400 kbps after 2 attempts |

The H.264 fixtures state `format=NV12` and nothing wider: an unpinned
`videotestsrc ! x264enc` negotiates High 4:4:4 10-bit on this box, which is not
what the encoder sends (4:2:0 8-bit) and not what any destination decodes.

### The capless audio pad

flvmux keeps a linked request pad in its collect-pad aggregate even after its
peer is unlinked. With no caps on that pad the muxer never completes
negotiation and emits nothing, so video backs up into the 4 MB appsrc (transcode:
through `vah264enc` first) and the attempt is rebuilt by the stall watchdog after
`k_stall_timeout`. Isolated on the same flvmux/vah264enc chain with a starved
audio appsrc: doing nothing yields 0 muxer buffers over the next 4 s, unlinking
the peer also yields 0, and unlinking then pushing EOS on the muxer's audio sink
pad yields 273.

No timer is involved in either tier's template: the audio branch ends at
`capsfilter name=acaps` and the muxer's audio pad is requested by
`output::attach_audio_branch` only when the source TS exposes an audio pad. A
video-only ingest never gives flvmux a pad to wait on, so video publishes on the
first encoded frame, and a second audio pad — or a link that does not take — is
sunk into the drop funnel instead of being left on the muxer.

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

## `image_resolver_proof.sh` — the image's address-family policy

    test/harness/image_resolver_proof.sh [--image obr-receiver:proof]

Not a publishing harness: it proves the runtime image's `/etc/gai.conf`
(`precedence ::ffff:0:0/96 100`) reorders a dual-stack name, with no
`--add-host` pin anywhere. It runs the same image twice on an IPv6 network —
once as built, once with `/etc/gai.conf` emptied — and requires IPv4 first in
the first run and IPv6 first in the second. The second run is what makes the
first one meaningful: on a single-stack container glibc has already demoted
AAAA (rule 3), so the file looks inert and a one-sided check proves nothing
(DECISIONS §20). The connect step opens a socket to the *first* address the
resolver returned, as a literal, which is the shape `rtmp2sink` has — one
address, no happy-eyeballs fallback.

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
`obu-stream,alignment=frame`). It writes a `<fixture>.meta` sidecar with the
frame count, so a reused fixture is fed through RIST at its real rate instead of
the harness's default frame count. The harness then prints, and gates on:

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

## standby_fanout_e2e.sh — one destination connection across slate and live (plan G1)

Proves the standby fan-out keeps **one** RTMP publish alive across slate → live
→ slate, across an encoder restart whose timestamps begin at the origin again,
and for a video-only ingest (the JPEG XS camera-link shape).

    test/harness/standby_fanout_e2e.sh                    # all three scenarios
    test/harness/standby_fanout_e2e.sh --scenarios "1"    # just the first
    SINK_HOST=10.50.1.51 test/harness/standby_fanout_e2e.sh

`38 s` to pass, `65 s` to fail (each scenario waits 20 s for the first
publish), no GPU, no container when `mediamtx` is installed. Ports come from the
environment (`CTRL_PORT`, `RIST_PORT`, `RTMP_PORT`, `SINK_API_PORT`, +10 per
scenario); the sink binds `SINK_HOST` because the receiver refuses loopback as
an egress target.

### Why the assertion is the sink's connection count

`bytesReceived` cannot see a reconnect: a destination that drops at every switch
and republishes a second later still shows a monotonically growing byte count.
Every publish prints one `is publishing to path 'live/e2e'` line in the sink log,
so the harness counts those:

- scenario 1: slate publishes before any RIST media exists; feed → `live`; stop →
  `slate`; **exactly one** publish line for the whole run, no `closed` line,
  `reconnects == 0`.
- scenario 2: feed, stop, then a *second, fresh* encoder (a new `gst-launch`, so
  its first PTS is the stream origin again — verified with `ffprobe` on the teed
  TS, `3600003 ms -> 3600003 ms`). One publish line, no disconnect, and the
  published byte counter never decreases across the switch.
- scenario 3: no audio in the ingest; the stream still goes live, keeps reaching
  the sink, and the muxer is not parked on an audio pad that never fills
  (`audio_frames > 0 || audio_dropped`).

Latency is measured from the receiver's own `last_live_ms_ago` dip, not from the
encoder's process start: the RIST Advanced profile drains ~1-2 s of buffered
media after the encoder dies, and `gst-launch` needs ~3 s to produce its first
frame. Measured here: slate→live `1.2-1.4 s`, live→slate `~1.2 s`.

### Known blocker at the time of writing

Against `fbc4dcf` all three scenarios fail at the first assertion — the slate
never publishes and the sink records zero publish connections. The muxer's audio
pad is linked (`audio es_ring present; muxer audio pad linked`) and then never
receives a buffer (`audio_frames: 0`), so flvmux never reaches PLAYING and
`rtmp2sink` never sends its publish command; MediaMTX closes the idle connection
after 10 s and the output reconnects. The cause is the ES audio framing: the
stage publishes **raw** AAC access units (`aacparse ! audio/mpeg,stream-format=raw`)
while the copy output's audio `appsrc` declares only `audio/mpeg,mpegversion=4`,
so the output's own `aacparse` tries to sniff ADTS on each raw frame, never sets
src caps and emits nothing. See the fix note in the task report; until it lands
this harness is a failing test that describes the product, not a regression.
