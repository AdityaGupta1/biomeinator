_Last edited: 2026-03-30_

# Render Passes

All passes are recorded into a single command list per frame and submitted together. The full sequence only runs when the TLAS exists and accumulation hasn't stopped (`stopAccumulating` is false or antialiasing isn't in accumulate mode). If neither condition is met, the CPU sleeps 3ms and only the postprocess pass runs.

## Pass Sequence

```
G-Buffer
  ↓
[RC Evict → RC Update → RC Resolve]  (only if rcEnabled)
  ↓
Path Tracing
  ↓
Collect
  ↓
[DLSS]  (only if antialiasingMode == DLSS)
  ↓
Postprocess  (or Debug View)
  ↓
ImGui
  ↓
Present
```

---

## G-Buffer (ray generation shader)

Traces one ray per pixel to find the primary hit. Writes a flat `GbufferData[]` buffer (`dev_gbuffer`) at render resolution — not a texture, just a raw structured buffer indexed by `pixelIdx`. The G-buffer is then transitioned to `NON_PIXEL_SHADER_RESOURCE` before the next passes read it.

The G-buffer also outputs the per-pixel data that DLSS needs: motion vectors, linear depth, normals+roughness, diffuse albedo, specular albedo, specular hit distance. These are written as UAV `RtTarget`s rather than into the `dev_gbuffer` buffer.

---

## Radiance Cache (optional, three sub-passes)

Runs between G-buffer and path tracing only when `rcEnabled`. The RC buffers (`dev_rcHashEntries`, `dev_rcAccumulation`, `dev_rcResolved`) are lazily created when RC is first enabled and destroyed when it's disabled.

**RC Evict** (compute) — ages/clears stale cache entries before new data is written.

**RC Update** (ray generation) — dispatches at `1/RC_UPDATE_SCALE` of render resolution. Traces rays from G-buffer hit points to gather indirect radiance into the hash table.

**RC Resolve** (compute) — finalizes hash entries into the `dev_rcResolved` buffer so the path tracing pass can read it as an SRV.

The RC buffers are transitioned UAV → SRV before path tracing reads them, then back to UAV afterward. When RC is disabled, `dev_rcStub` (a tiny dummy buffer) is bound in its place so the path tracing shader always has valid SRVs.

---

## Path Tracing (ray generation shader)

The main rendering pass. Reads the G-buffer to start paths from primary hits rather than from the camera, which avoids re-tracing primary rays. Writes raw accumulated HDR radiance into two raw buffers (not textures): `dev_pathTracingRawBuffer` and `dev_ptDiffuseAlbedoRawBuffer`.

When `doPathSplitting` is on, the dispatch width is doubled — the extra columns handle specular paths separately from diffuse paths.

---

## Collect (compute shader)

Reads the raw PT buffers and resolves them into the actual `RtTarget` textures (`pathTracingTarget`, `diffuseAlbedoTarget`). Handles temporal accumulation (adding to running average across frames) and applies tonemapping. The result in `pathTracingTarget` is what gets displayed (or fed to DLSS).

The raw buffers exist because accumulation needs to work in linear HDR — you can't accumulate into a tonemapped texture. When path splitting is enabled, collect is also where the split diffuse and specular paths are recombined into a single output.

---

## DLSS (Streamline, optional)

`slEvaluateFeature(sl::kFeatureDLSS_RR, ...)` upscales `pathTracingTarget` (at render resolution) to `dlssOutputTarget` (at viewport/display resolution). The input buffers were tagged at the start of the frame via `slSetTagForFrame`. When DLSS is active, `renderParams.preTonemappedColorSrvIdx` points to `dlssOutputTarget` instead of `pathTracingTarget`, so postprocess reads the upscaled image.

---

## Postprocess / Debug View (rasterization)

A full-screen triangle (3 vertices, no vertex buffer) draws to the swap chain back buffer. Normally uses the postprocess pipeline, which samples `preTonemappedColorSrvIdx` (already tonemapped by collect) and outputs it. If a debug view is active, the debug view pipeline is used instead, which visualises a selected `RtTarget` or RC buffer with optional scale and tonemapping.

ImGui is rendered on top of the postprocess output before the back buffer transitions back to `PRESENT`.

---

## Accumulation Reset

`accumulatedFrameNumber` resets to 0 whenever the camera moves, the scene changes, or a path-tracing setting changes (`didPathTracingSettingsChange`). Accumulation stops entirely (`stopAccumulating = true`) when `maxAccumulatedFrames` is reached — at that point in test mode a screenshot is automatically captured.

`didPathTracingSettingsChange` should only be set for settings that actually alter the accumulated linear radiance — i.e. settings that change what the path tracer computes, such as `maxPathDepth` or `samplingMode`. Settings that only affect post-processing of the final result (e.g. `tonemapping`) should not trigger a reset, since the underlying accumulated radiance is still valid.
