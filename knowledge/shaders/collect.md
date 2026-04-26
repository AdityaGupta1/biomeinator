_Last edited: 2026-04-26_

# Collect Shader

`src/shaders/path_tracing/collect.cs.hlsl` — compute shader that bridges raw path tracing
output buffers to `RtTarget` textures consumed by DLSS and postprocess. See
[render_passes.md](../rendering/render_passes.md) for where this sits in the frame.

## Why It Exists

The path tracer writes to flat structured buffers rather than directly to textures because:
- Accumulation needs to average in linear HDR — can't accumulate into a tonemapped texture.
- Path splitting doubles the buffer width (two entries per pixel), and collect is where
  split paths are recombined.

Accumulation-mode averaging is only applied here, not in DLSS mode — DLSS handles temporal
integration itself.
