_Last edited: 2026-10-03_

# Render Loop

`Renderer::render()` in `renderer.cpp` runs one frame. See
[render_passes.md](render_passes.md) for the pass sequence within a frame.

## Idle Short-Circuit

When there's no TLAS or accumulation has stopped, the entire ray tracing + collect section
is skipped and CPU sleeps 3ms to avoid spinning. Only postprocess runs (re-presents the
previous frame's result).

## Minimized Window

`render()` returns early (after a short sleep) while the client area is empty, before any
Streamline call. Running a frame there would resize to the 1x1 clamp in `resize()`, and DLSS-RR
rejects that output size with `eErrorNGXFailed` on evaluate. The check is on the client rect, not
`IsIconic`: on restore the minimized state clears before the client area regains its size, so an
`IsIconic` guard still lets one 1x1 resize through.

## Frame Pacing

Two mechanisms: waitable swap chain (`WaitForSingleObjectEx` on the swap chain latency
handle) and fence wait on the current frame context. Waitable swap chain is optional
(`useWaitableSwapChain` setting) and controls input-to-display latency. The fence wait is
always active and prevents overwriting a frame context the GPU is still reading.

## Accumulation Reset Triggers

`resetAccumulation` fires when camera moves, scene changes, OR `didPathTracingSettingsChange`
is set. Only settings that alter the path-traced radiance should set this flag — post-process
settings (tonemapping, debug view) should not, since the accumulated HDR data is still valid.

## Animation Time and Input

Keyboard input in `window_manager.cpp` splits into two paths, and new controls must pick the
right one: **edge-triggered** (`onKeyDown`) for toggles and one-shot actions, **polled per
frame** (`getPlayerInput`, `getTimeScrubDirection`) for anything continuous.

Animation time scale resolves in strict precedence: **scrub → `animTimePaused` setting → 1x**,
so a scrub key steps time even while paused.

Gotchas:

- Pause lives in the `animTimePaused` setting, not renderer state, so the key, the CLI, and the
  test runner all drive one switch with no separate pause path.
- Scrub direction is sampled *before* `getPlayerInput()` in `render()`, so `lockCamera` (which
  zeroes `PlayerInput` wholesale) does not disable time control.

## Automated Runs

`--renderToFile` (screenshot for rendering tests and agents) and `--perfOutput` (timing report)
both make the run *automated*: Streamline logging is suppressed and in voxel mode the world
import is awaited before anything counts. `renderState.automatedRun` gates those shared
behaviours, and `SettingsManager` defaults the camera lock, GUI, animation pause, vsync and
SHaRC for both (see [settings → settings_manager.md](../settings/settings_manager.md)).
`renderState.renderToFileMode` gates the render-to-file exit, where accumulation runs to
`maxAccumulatedFrames` then auto-captures a screenshot and exits, plus the things only a
render-to-file run gives up: frame generation and a visible window. The window is created but
never shown, so agents can render images without windows popping up; the capture copies the
back buffer before `Present`, and presenting to a hidden window returns the success code
`DXGI_STATUS_OCCLUDED`, so nothing downstream depends on the window being visible. The
perf lifecycle is separate and independent of accumulation; see
[tests → perf_runs.md](../tests/perf_runs.md).

Every frame is bracketed by `GpuProfiler::beginFrame`/`endFrame` and the passes are wrapped in
profiler scopes, which always emit PIX markers but only record timestamps in perf mode; see
[gpu_profiler.md](gpu_profiler.md) for where scopes may and may not go.
