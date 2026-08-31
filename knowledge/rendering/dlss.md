_Last edited: 2026-08-30_

# DLSS

DLSS Ray Reconstruction (DLSS-RR / DLSS-D) via NVIDIA Streamline SDK. Upscales from render
resolution to viewport resolution while denoising the path-traced output.

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

## Mip Bias

When rendering at lower resolution, texture mips need a negative bias to compensate
(`log2(renderWidth / viewportWidth) - 1`). This is passed to shaders via `renderParams`.
