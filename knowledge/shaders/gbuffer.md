_Last edited: 2026-08-30_

# G-Buffer Shader

`src/shaders/path_tracing/gbuffer.rgs.hlsl` — ray generation shader that traces primary
rays and outputs the G-buffer plus DLSS guide buffers. See
[render_passes.md](../rendering/render_passes.md) for where this sits in the frame.

## Why a Separate Pass

Primary rays are separated from the main path tracer so that:
1. DLSS guide buffers can be written at render resolution from just the first hit.
2. The path tracer avoids re-tracing primary rays — it reads `GbufferData` directly.

## Non-Obvious Details

**Motion vectors**: `worldToPrevClipMat` already accounts for `globalInstanceOffset` changes
between frames, so no manual offset correction is needed in this shader. Water-top hits
additionally reconstruct the previous frame's surface position analytically — displacement
is vertical at fixed XZ, so re-evaluating `waveHeight` at `renderParams.prevTime` gives the
previous Y. Refracted geometry seen through water keeps camera-only motion. The noise-based
normal perturbation is shading-only and intentionally ignored.

**Diffuse albedo** is intentionally NOT written here — it's written by the path tracing
shader because specular bounces modulate it (a specular first bounce looks through to the
second hit's base color).
