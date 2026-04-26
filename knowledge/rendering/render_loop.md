_Last edited: 2026-04-26_

# Render Loop

`Renderer::render()` in `renderer.cpp` runs one frame. See
[render_passes.md](render_passes.md) for the pass sequence within a frame.

## Idle Short-Circuit

When there's no TLAS or accumulation has stopped, the entire ray tracing + collect section
is skipped and CPU sleeps 3ms to avoid spinning. Only postprocess runs (re-presents the
previous frame's result).

## Frame Pacing

Two mechanisms: waitable swap chain (`WaitForSingleObjectEx` on the swap chain latency
handle) and fence wait on the current frame context. Waitable swap chain is optional
(`useWaitableSwapChain` setting) and controls input-to-display latency. The fence wait is
always active and prevents overwriting a frame context the GPU is still reading.

## Accumulation Reset Triggers

`resetAccumulation` fires when camera moves, scene changes, OR `didPathTracingSettingsChange`
is set. Only settings that alter the path-traced radiance should set this flag — post-process
settings (tonemapping, debug view) should not, since the accumulated HDR data is still valid.

## Test Mode

When `testMode` is active (`testOutput` setting is non-empty), accumulation runs to
`maxAccumulatedFrames` then auto-captures a screenshot and exits. Streamline logging is
suppressed and the window is not brought to foreground.
