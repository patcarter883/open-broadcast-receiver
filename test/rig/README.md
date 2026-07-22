# Real-media rigs

Two containerised end-to-end harnesses:

- **`smoke.sh`** — single-path **real-media smoke** (FIXPLAN M4.1), runs
  **per-PR**. The fast proof of the core product function.
- **`rig.sh`** — the full **bonding** soak (M1.6/M1.8), runs **nightly**.

## Single-path smoke (`smoke.sh`)

```
sender (gst test src → rist_feed) ─RIST─▶ receiver (hosted profile)
   ─rtmp1─▶ SRS ◀─ffprobe/ffmpeg─ prober
```

One path, one RTMP output, no netem — ~30 s after the images build. Asserts:
- the receiver ingested RIST (`rist.received > 0`) and the RTMP output is
  `running` and `bytes_sent > 0`;
- the SRS stream is **playable** — `ffprobe` sees an **h264** video + **aac**
  audio stream (proves the copy-only fan-out + flvmux remux produced valid RTMP);
- it actually **decodes** — `ffmpeg` reads 3 s to a null sink without error.

```sh
test/rig/smoke.sh
```

CI: `.github/workflows/smoke-rig.yml` (pull_request + push + manual). This is
the gate that proves real video reaches a real RTMP endpoint; the bonding rig
below adds multi-path resilience on top.

---

# Bonding CI rig (FIXPLAN M1.6 + M1.8)

Containerised end-to-end harness for the bonded topology — the test that would
have caught C1 (timing-mode SIGABRT) and H1 (port-fan bonding):

```
sender (gst test src → rist_feed) ─lan─▶ bond (rist2rist, 2 netem WANs)
   ─wan_a(40ms±10ms, 2% loss)─┬─▶ receiver :5000  ── rtmp1 → SRS #1
   ─wan_b(120ms±30ms, 5% loss)┘   (ONE port,        rtmp2 → SRS #2
                                   TWO peers)       srt   → listener
                                                    recording
```

## Stages (`rig.sh`)

1. **Red-proof** — receiver image built with `TIMING_MODE=1` (ARRIVAL) must
   **crash** under the bonded double hop within 2 minutes. This proves the rig
   detects the original defect; only then is `timing-mode=0` pinned. (The rig
   deliberately uses a Debug build so librist's asserts are armed.)
2. **Soak** — `timing-mode=0`, default 900 s, with continuous and one-shot
   assertions:
   - no container leaves `running` state for the whole soak (C1 regression);
   - `/stats` shows **two peers on the single session port** (H1 topology);
   - 10 s full outage on wan_a → traffic shifts to peer B, flow survives;
   - **CGNAT churn**: conntrack flush + bond restart forces source-address
     rebinding mid-stream → flow must survive (H1's open librist question,
     answered empirically);
   - **drop-storm (M1.8)**: SRS #1 paused 45 s → that consumer takes ring
     drops (`dropped_bytes` accounted), then must recover to `running`
     (flvmux tolerates the forward timestamp jump) while srt/rtmp2 continue
     with zero drops;
   - final: `recovered > 0` (netem loss exercised RIST retransmission),
     recording grew, session still `running`, clean `/stop`.

## Running

```sh
test/rig/rig.sh                       # full: red-proof + 900 s soak
RIG_DURATION=120 SKIP_RED=1 test/rig/rig.sh   # quick local iteration
```

Requires docker compose and the NET_ADMIN capability for the bond container
(netem). CI: `.github/workflows/bonding-rig.yml` (nightly + manual dispatch).

Caveats:
- Rig images use the distro GStreamer (1.24 on noble); production floor is
  1.28. Bump the base images when a 1.28 base exists.
- The M1.5 passthrough-pacing capture comparison can be run on this topology
  (capture at the srt listener) but is a separate exercise.
