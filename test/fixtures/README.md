# Checked-in fixtures

## `av1-640x360.ts`

A 3 s (90 frames @ 30 fps), 640x360 AV1-in-MPEG-TS clip with no audio track.
It carries AV1 the way the pipeline produces it: `mpegtsmux
alignment=7 enable-custom-mappings=true`, i.e. stream type `0x06` with the
`AV1G` registration descriptor rather than a standardised AV1 stream type.

`test/harness/receiver_e2e_guard.sh` (ctest `receiver_e2e_guard`) uses this
file only when no AV1 encoder element is installed; otherwise it generates its
own fixture into the build directory. The `.meta` sidecar records the frame
count so the guard feeds the clip at its real rate.

Regenerate with:

```sh
FIXTURE_WIDTH=640 FIXTURE_HEIGHT=360 FIXTURE_FRAMES=90 FIXTURE_BITRATE=800 \
FIXTURE_AUDIO=0 \
  test/tools/make-av1-ts-fixture.sh test/fixtures/av1-640x360.ts
```

The script verifies the result with `tsdemux`/`av1parse` before exiting 0. The
frame count in the filename and the `.meta` sidecar must stay in step with the
guard's default of 90 frames.
