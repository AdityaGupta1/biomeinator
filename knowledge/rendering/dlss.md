_Last edited: 2026-04-27_

# DLSS

DLSS Ray Reconstruction (DLSS-RR / DLSS-D) via NVIDIA Streamline SDK. Upscales from render
resolution to viewport resolution while denoising the path-traced output.

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
