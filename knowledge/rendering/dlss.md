_Last edited: 2026-09-08_

# DLSS

DLSS Ray Reconstruction (DLSS-RR / DLSS-D) via NVIDIA Streamline SDK. Upscales from render
resolution to viewport resolution while denoising the path-traced output.

DLSS Frame Generation (DLSS-G / DLSS-FG) sits on top of it, interpolating one extra frame per
rendered frame. It is on by default, gated on the `frameGeneration` setting.

## Manual Hooking

Streamline is integrated using **manual hooking** (`PreferenceFlags::eUseManualHooking`).
This avoids routing every D3D12/DXGI call through SL proxy objects — only the calls SL
actually intercepts go through proxies. This reduces CPU overhead and prevents third-party
libraries (NVAPI, ImGui) from receiving unexpected proxy interfaces.

`RendererState` stores both native and proxy variants of `device`, `swapChain`, and
`factory`. The native variant is the primary one used by most code. The proxy variants
(`proxyDevice`, `proxySwapChain`, `proxyFactory`) exist only for the hooked calls listed in
`sl_hooks.h`:

- **Factory proxy**: `CreateSwapChainForHwnd` (swapchain creation)
- **Device proxy**: `CreateCommandQueue`
- **SwapChain proxy**: `Present`, `GetBuffer`, `ResizeBuffers`, `GetCurrentBackBufferIndex`

Everything else — descriptor heaps, root signatures, PSOs, render target views, NVAPI,
ImGui — uses the native interfaces. Adding a new D3D12 call site almost always means using
the native `renderState.device`; only add a proxy call if `sl_hooks.h` lists that specific
API.

During teardown, release proxy interfaces before their native counterparts (e.g.
`proxySwapChain` before `swapChain`, `proxyDevice` before `device`). The proxy wraps the
native object, so the native ref must outlive the proxy.

## Render vs Viewport Resolution

When DLSS is active, `renderWidth/Height` is smaller than `viewportWidth/Height` —
Streamline's `slDLSSDGetOptimalSettings` determines the optimal render resolution for the
selected quality mode. All ray tracing and G-buffer work happens at render resolution; DLSS
upscales to viewport resolution. When DLSS is off, render = viewport.

## Resource Tagging

Streamline requires tagging input resources (`slSetTagForFrame`) each frame BEFORE the
command list work that produces them. This is why resource tagging happens at the top of
`render()`, before any dispatches.

## Reset Signal

`DlssState::needsReset` (in `renderState.dlss`) is set on scene load and resize. Passes
`reset = eTrue` in `sl::Constants` for one frame, telling DLSS to discard temporal history.

## Frame Generation

Since the setting defaults to on, the first `render()` call normally enables frame generation and
therefore rebuilds the swap chain once at startup. That is deliberate: routing startup through the
same `setFrameGenerationActive` path as a runtime toggle keeps one code path for enabling it,
at the cost of one extra swap chain create and target reallocation before the first frame.

Frame generation is only offered when DLSS-G, Reflex and PCL all report support, which is checked
once at startup (`initFrameGenSupport`). The usual reason for it to be unavailable is Windows
Hardware-accelerated GPU Scheduling being off, which no amount of application-side work can fix.
Headless runs opt out entirely: generated frames would corrupt golden screenshots, and Reflex
pacing the frame start would skew perf measurements.

Reflex is not optional — DLSS-G refuses to run without it, and an ordinary NVAPI Reflex
integration does not count, it has to be Streamline's. The PCL markers around simulation, render
submit and present are what carry the frame index DLSS-G matches its tagged inputs against; if
they drift out of sync the log fills with `common constants cannot be found for frame N`. Because
of that the markers and `slReflexSleep` run whenever frame generation is *supported*, not only
while it is switched on, so the pairing is already correct at the moment it gets enabled.

### Toggling recreates the swap chain

While the sl.dlss_g plugin is loaded, the application renders to an off-screen target and SL owns
the real swap chain. Leaving the plugin loaded with frame generation merely set to `eOff` keeps
that extra copy and the cross-queue sync, so `setFrameGenerationActive` unloads the plugin and
rebuilds the swap chain around the new state. This is why the DXGI factories are kept alive past
`initSwapChain` instead of being released there.

Loading the plugin is also not sufficient on its own: interpolation only starts once
`slDLSSGSetOptions` sets the mode.

### Buffer states at Present

DLSS-RR consumes its tagged inputs during `slEvaluateFeature`, partway through the command list.
DLSS-G consumes its own at Present, *after* the postprocess pass has moved every auto-transitioned
target to `PIXEL_SHADER_RESOURCE`. A resource has one tag but is read at both moments, so the
motion vector and NDC depth targets are transitioned back to `UNORDERED_ACCESS` at the end of the
frame to match what their tags declared.

### The waitable object

DXGI gives one frame-latency waitable object per swap chain and only one side may wait on it. With
the plugin loaded the app drops the flag entirely and lets SL pace, using `slReflexSleep` in place
of its own wait; `isWaitableSwapChainActive()` is the single place that decision is expressed, and
it gates swap chain creation, resize and the frame-start wait together.

### Depth

DLSS-G requires post-projection depth (`kBufferTypeDepth`). `linearDepthTarget` cannot serve: it
holds ray distance, not even view-space Z, and only DLSS-RR's `kBufferTypeLinearDepth` tolerates
that. `ndcDepthTarget` exists for frame generation and is written alongside it in the G-buffer
raygen shader.

### Reporting frame rate

The FPS counter accumulates `DLSSGState::numFramesActuallyPresented` rather than counting
`render()` calls or multiplying by the frame multiplier. Scaling is not a clean 2x in either
direction: DLSS-G drops the interpolated frame when presents go out of sync, and a CPU-bound host
can exceed 2x because `Present` stops blocking it. The value counts frames since the previous
`slDLSSGGetState`, which is one app frame here only because `updateFrameGenState` is called once
per frame right after Present.

The frame time graph stays rendered frame time, so it keeps lining up with the GPU profiler
scopes — which is why the FPS readout shows the rendered figure alongside the presented one, and
why the two disagree by design.

### Hudless

`hudlessTarget` is a copy of the back buffer taken after the postprocess draw and before ImGui, so
interpolation sees the scene without the overlay smeared across it. Its format has to track the
swap chain's. No UI alpha buffer is tagged yet, so the overlay still degrades somewhat on generated
frames, and fullscreen menu auto-detection (which needs `kBufferTypeUIColorAndAlpha`) is
unavailable.

## Mip Bias

When rendering at lower resolution, texture mips need a negative bias to compensate
(`log2(renderWidth / viewportWidth) - 1`). This is passed to shaders via `renderParams`.
