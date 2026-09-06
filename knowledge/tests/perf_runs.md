_Last edited: 2026-09-06_

# Perf Runs

`--perfOutput=<path.json>` turns a launch into a measurement: wait for the scene, warm up,
measure a fixed number of frames, write per-scope GPU timing statistics, exit. It is the
programmatic way to check whether a change made rendering faster, and the intended workflow
for agents doing performance work. `tests/run_perf.py` drives it over the entries in
`tests/perf_scenes.json`; `run` produces a directory of reports, `compare` diffs two such directories
by median, `show` prints one.

```
python tests/run_perf.py run -o build/perf_output/baseline
# make the change, rebuild RelWithDebInfo
python tests/run_perf.py run -o build/perf_output/candidate
python tests/run_perf.py compare build/perf_output/baseline build/perf_output/candidate
```

Anything after `run`'s own options is passed through to every launch, e.g. `--perfFrames=1000`.

## Defaults, and which one actually binds

| setting | default |
|---|---|
| `perfWarmupFrames` | 100 |
| `perfWarmupSeconds` | 2 |
| `perfFrames` | 300 |
| `perfTimeoutSeconds` | 120 |
| quiet streak (`PERF_QUIET_FRAMES`) | 30 |

Warmup needs all of the first two plus the quiet streak, so at the frame rates these scenes
run at, **the 2 seconds is the binding constraint**: an 8 ms scene starts measuring around
frame 220, a 4 ms scene around frame 470, and only a 20 ms scene is held by the 100-frame
minimum. The consequence is that 300 measured frames on a fast scene is barely over a second
of data. `--perfFrames` is the lever when the ~10% run-to-run variance (see Noise) is too
loose for the delta being checked; warmup rarely needs touching.

## Lifecycle

`renderer_perf.cpp` runs a small state machine, advanced once per frame from `render()`:

- **Waiting for scene** ends when a TLAS exists and, in voxel mode, the world import has
  finished (the same gate the golden tests use).
- **Warmup** ends when *all* of `perfWarmupFrames`, `perfWarmupSeconds`, and a streak of
  `PERF_QUIET_FRAMES` frames without a scene change have been satisfied. The frame count covers
  one-time costs (PSO warmup, DLSS feature creation); the time covers voxel worlds still
  streaming boundary chunks after the import gate opens; the quiet streak catches whatever the
  other two miss.
- **Measuring** lasts exactly `perfFrames` frames, so A/B runs have the same sample size and
  the same per-frame work. GPU timings arrive `NUM_FRAMES_IN_FLIGHT` frames late, so frames
  are attributed by number against `[measureStartFrame, measureEndFrame)`, not by when their
  readback happened. The CPU sample for a frame is taken from after the frame-latency and
  fence waits up to just before `Present`, so it is the frame's own work with the throttling
  waits excluded, and it covers the same frames as the GPU samples. It is *not* the frame
  rate; that is what `gpu.frameMs` approximates when the GPU is the bottleneck.
- **Done** flushes the queue, drains the in-flight slots, writes the JSON, and exits through
  `Renderer::destroy` like a golden run does. `perfTimeoutSeconds` bounds the whole run; on
  timeout the report is still written with whatever was measured and `meta.timedOut` set, but
  the process exits non-zero, `run` reports the entry as failed, and `compare` refuses to diff
  it. The partial report is for diagnosing the timeout, not for comparison.

Perf mode is a *headless* run, sharing that flag with `--testOutput`: camera locked, GUI
hidden, animation paused, vsync off, no foreground window, Streamline logging off, voxel
import awaited. `SettingsManager::isHeadless()` is the switch for those; `isTestMode()` stays
specific to the golden screenshot-and-exit path. The headless defaults (`lockCamera`,
`showGui`, `animTimePaused`, `useVsync`) live in `parseArgs` and are only applied when the
flag was not passed explicitly, so a run can opt back into animation if it wants moving water
in the measurement.

## Why DLSS mode, not accumulate mode

`perf_scenes.json` entries use `--antialiasingMode=2` because accumulate mode idles once
`maxAccumulatedFrames` is reached and skips the denoiser, while DLSS mode renders every frame
and is what the voxel game actually runs. The perf lifecycle is independent of the accumulation
counter either way.

## Noise

- **`SetStablePowerState(TRUE)`** is called in perf mode and locks GPU clocks to base. It
  requires Windows developer mode, and without it the call does not merely fail: it returns
  `E_FAIL` and then removes the device, so the next resource creation dies with
  `DXGI_ERROR_DEVICE_REMOVED`. `perfRunInit` therefore checks the developer-mode registry key
  first and only warns when it is off, leaving `meta.stablePowerState` false. Absolute numbers
  under stable power are lower than a real run's; deltas are what matter.
- **Vsync and present throttling.** The app only ever uses borderless windowed fullscreen, and
  tearing is passed whenever the driver supports it, so with `useVsync=false` presents are not
  capped at the refresh rate. If frame time in a report sits suspiciously close to a refresh
  interval, check that first.
- **Path tracing varies ~10% between back-to-back runs** on the same build (evil_room, RTX 4070
  SUPER, 300 frames). Treat single-digit deltas in a single scope as noise unless they
  reproduce; a real change looks like the SER sanity check below.
- **Light tree build and its sort are one scope**, see
  [rendering → gpu_profiler.md](../rendering/gpu_profiler.md).

## Sanity check

Commenting out `NvReorderThread` in `path_tracing.rgs.hlsl` and rerunning `evil_room` moved
only the path tracing scope, which is the one shader that calls it:

| scope | with SER | without SER |
|---|---|---|
| path tracing | 5.0 ms | 17.3 ms |
| gbuffer | 0.34 ms | 0.34 ms |
| dlss | 2.2 ms | 2.2 ms |

If a future change to the profiler breaks attribution, this is the quickest way to notice.

## Adding a scene

Entries in `tests/perf_scenes.json` have the same shape as `tests/tests.json` minus the golden:
`name`, one of `scene`/`world`, and `args`. Pin `width` and `height` in `args` (and `dlssMode`
if the default balanced preset is not wanted); DLSS render resolution derives from them and
the report records the resolved values in `meta`.
