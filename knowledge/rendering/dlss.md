_Last edited: 2026-09-08_

# DLSS

DLSS Ray Reconstruction (DLSS-RR / DLSS-D) via NVIDIA Streamline SDK. Upscales from render
resolution to viewport resolution while denoising the path-traced output.

DLSS Frame Generation (DLSS-G / DLSS-FG) sits on top of it, interpolating one extra frame per
rendered frame. It is on by default, gated on the `frameGeneration` setting *and* on DLSS being the
antialiasing mode (`isFrameGenerationRequested`). DLSS-G could run without DLSS-RR, but its checkbox
lives under the DLSS mode dropdown, so leaving it active in the other modes would give the user no
visible way to turn it off. Only the active state is gated; the setting keeps the user's choice for
when they switch back.

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

## Ray Reconstruction Preset

The `dlssPreset` setting applies one Ray Reconstruction preset to every DLSS quality mode. The UI
offers preset D, the previous Streamline SDK's default transformer model, and preset F, the current
SDK's latest/default transformer model. Preset F is the application default; keeping D selectable
makes before/after image-quality comparisons possible. Changing it follows the existing resize path
so Streamline receives the new preset while its temporal history and render targets are reset.

## Frame Generation

Frame generation is only offered when DLSS-G, Reflex and PCL all report support, which is checked
once at startup (`initFrameGenSupport`). Streamline reports *why* a feature is unavailable (pre-Ada
GPU, Hardware-accelerated GPU Scheduling off, old driver, old OS), and each calls for a different
user action, so `slResultToString` translates the code and the GUI shows it in place of the checkbox
rather than blaming one cause for all of them.
Headless runs opt out entirely: generated frames would corrupt golden screenshots, and Reflex
pacing the frame start would skew perf measurements.

Reflex is not optional — DLSS-G refuses to run without it, and an ordinary NVAPI Reflex
integration does not count, it has to be Streamline's. The PCL markers around simulation, render
submit and present are what carry the frame index DLSS-G matches its tagged inputs against; if
they drift out of sync the log fills with `common constants cannot be found for frame N`. Because
of that the markers and `slReflexSleep` run whenever frame generation is *supported*, not only
while it is switched on, so the pairing is already correct at the moment it gets enabled.

PCL Stats also posts a private window message to time how long input takes to reach a frame. The
message pump only raises a flag (`queuePclPing`); the ping marker itself goes out with the next
frame's token, since that is the frame that picks up the input. Requesting a separate token for
the ping would burn one of SL's few in-flight token slots.

### Plugin load vs. interpolation

While the sl.dlss_g plugin is loaded, the application renders to an off-screen target and SL owns
the real swap chain. Leaving the plugin loaded with frame generation merely set to `eOff` keeps
that extra copy and the cross-queue sync, so the plugin is only loaded while the setting is on and
every load or unload is followed by a swap chain rebuild. At startup `initSwapChain` loads it
before the first swap chain exists, so the default-on setting costs no rebuild; a runtime toggle
goes through `setFrameGenerationActive`, which rebuilds the swap chain and queues a resize rather
than running one, so a frame that also has a pending resize still resizes once. This is why the
DXGI factories are kept alive past `initSwapChain` instead of being released there.

Loading the plugin is not sufficient on its own: interpolation only starts once `slDLSSGSetOptions`
sets the mode. That happens at the end of `resize()`, the one place that follows every swap chain
creation. `resize()` also turns the mode off before `ResizeBuffers`: while interpolating, DLSS-G
presents from its own thread, and section 12 of its guide requires it to be off across any window
manipulation to avoid a deadlock.

DLSS-G fails quietly, still presenting real frames (with a pink overlay) and reporting the reason
only through `DLSSGState::status`. A bad status turns the `frameGeneration` setting off rather than
just logging, as section 14.3 of the guide asks; using the setting keeps the GUI checkbox honest and
lets the user try again. The resolution-too-low status is how a tiny window is handled.

`hudlessTarget` is only allocated while frame generation is on. Toggling queues a resize anyway, so
gating it in the resize loop costs nothing and frees a full-resolution RGBA8 texture on the off path.

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

Both features read one `depthTarget` holding post-projection depth, tagged as `kBufferTypeDepth`.
DLSS-RR would also accept view-space Z under `kBufferTypeLinearDepth`, but the ray *distance* the
G-buffer naturally produces is neither, so the earlier linear depth target was subtly off-spec and
was dropped once frame generation needed NDC depth anyway. The projection is standard non-reversed
Z with a 0.1 / 10000 near/far ratio, so distant depth is coarse; DLSS only uses depth for
disocclusion and history rejection, but if distant RR quality ever suffers, reversed-Z with
`depthInverted` set is the fix.

On a miss, the G-buffer shader places the sky on the far plane itself (ray distance
`farPlane / dot(dir, forward)`), not on a sphere of radius `farPlane`, so sky depth is 1 across the
whole frame instead of falling off towards the edges.

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
interpolation sees the scene without the overlay smeared across it. It uses `SWAP_CHAIN_FORMAT`,
the same constant the swap chain, the postprocess PSO, ImGui and the screenshot readback use, since
`CopyResource` needs the formats to match exactly. It keeps its SRV flag even though no shader
reads it: DLSS-G creates its own views on the resource, and `DENY_SHADER_RESOURCE` would break
that. No UI alpha buffer is tagged yet, so the overlay still degrades somewhat on generated
frames, and fullscreen menu auto-detection (which needs `kBufferTypeUIColorAndAlpha`) is
unavailable.

## Mip Bias

When rendering at lower resolution, texture mips need a negative bias to compensate
(`log2(renderWidth / viewportWidth) - 1`). This is passed to shaders via `renderParams`.
