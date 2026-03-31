_Last edited: 2026-03-31_

# Path Tracing Shader

`src/shaders/path_tracing.rgs.hlsl` — the main rendering shader. Compiled twice: once normally for the path tracing pass, and once with `RC_UPDATE` defined for the radiance cache update pass. The same `pathTraceRay()` function drives both, with `#ifdef RC_UPDATE` blocks controlling whether radiance is written to the output buffers or to the radiance cache.

This shader does NOT trace primary rays. It reads the G-buffer to get the primary hit, then traces secondary rays from there. See [render_passes.md](../rendering/render_passes.md) for the full pass sequence.

---

## Shared Code

`path_tracing_common.hlsli` is shared by this shader, the G-buffer shader, and the light sampling code. It declares the TLAS, vertex/index/instance/per-tri buffers, and provides:

- `getPixelIdx()` / `getPathSplitIdx()` — when path splitting is enabled, the dispatch width is doubled, and even/odd X dispatch indices map to split index 0/1 for the same pixel.
- `AnyHit` — handles two things: (1) alpha cutout via stochastic alpha testing, and (2) refraction passthrough, where transmissive delta surfaces are skipped via `IgnoreHit()` while accumulating path weight and tracking water entry/exit T values.
- `ClosestHit_Primary` — standard hit recording: world-space position, normal (oriented to face the ray origin — i.e. flipped on backfaces), UV, instance/triangle IDs, material index.

---

## Path Tracing Loop

`pathTraceRay()` implements the full path tracing loop. Here is the high-level flow:

### Initialization

The G-buffer provides the first hit. Before the loop starts:
1. Compute **segment absorption** between the camera and the primary hit (handles underwater camera).
2. If the primary ray missed, return the **dome light** color (voxel mode only; returns black in glTF mode).

### Per-Bounce

Each iteration of the loop represents one bounce, up to `effectiveMaxPathDepth`:

1. **Emission** — if the surface emits light, add its contribution weighted by `pathWeight`. On the first bounce, only `pathSplitIdx == 0` handles emission to avoid double-counting when path splitting.

2. **Path splitting** (first bounce only, non-RC_UPDATE) — `trySplitMaterial()` deterministically splits the material into two lobes across the two path split indices. Two kinds of splits:
   - **Alpha transparency**: split 0 = opaque (weighted by alpha), split 1 = transparent passthrough (weighted by 1-alpha).
   - **Fresnel**: split 0 = diffuse/transmission + emission (weighted by 1-F), split 1 = specular reflection (weighted by F).
   If the material can't be split, split index 1 early-returns.

3. **Passthrough check** — after `refractionIndirectPassthrough` is enabled and a non-delta surface has been encountered, delta transmissive surfaces are treated as passthrough: the path weight is tinted by the base color, but the position/normal from the previous "real" bounce are preserved. The motivation is noise reduction when sampling lights through surfaces like water — shadow rays use the same passthrough logic, so indirect paths and direct estimates stay consistent. This sacrifices some accuracy but is worth it for real-time.

4. **SER reordering** — `NvReorderThread()` sorts threads by a coherence hint (first bounce, passthrough, or scattering non-delta surface) to improve warp occupancy.

5. **Russian roulette** — from depth 2 onward, paths may be terminated probabilistically based on luminance of `pathWeight`, with a minimum 10% survival probability.

6. **BSDF sampling** — `sampleBsdf()` picks a direction:
   - **Specular reflection**: perfect mirror via `reflect()`, weighted by `glossyReflectionTint * fresnelReflectance`.
   - **Glossy transmission**: `refract()` through the surface, weighted by base color.
   - **Diffuse**: cosine-weighted hemisphere sampling with Lambertian BRDF, accounting for Fresnel reflection loss.

   The Fresnel decision (reflect vs transmit/diffuse) is stochastic — a random number is compared against the Walter Fresnel reflectance.

7. **Direct light sampling** (MIS/RIS only, non-delta surfaces) — two independent strategies, both MIS-weighted against the BSDF sample using the balance heuristic:

   - **Area lights**: in MIS mode, one light is picked uniformly and a shadow ray is traced. In RIS mode, multiple candidate lights are resampled to pick one with importance proportional to a target function, then a shadow ray is traced. The MIS weight uses the solid-angle pdf of the chosen light.

   - **Dome light** (voxel mode only): a direction is sampled uniformly within the sun's spherical cap and a shadow ray is traced. If it misses all geometry, the dome light radiance (sun or sky gradient) is added. This is separate from area light sampling because the two can't produce each other's samples (dome light can't hit area lights and vice versa), so their MIS weights are independent.

8. **Trace next ray** — `TraceRay` from the BSDF-sampled direction. Update material, ray cone width, segment absorption.

9. **Radiance cache early termination** (non-RC_UPDATE, depth >= 1) — if the hit point maps to a valid, sufficiently-sampled RC cell, return early with the cached radiance. Requires the *previous* bounce to be non-delta (not purely specular), and the hit distance and ray cone width to both exceed the RC voxel size.

10. **BSDF-hit emission MIS** — if the BSDF-sampled ray hit an emissive surface, apply MIS weighting against the light sampling pdf (only for non-specular bounces, since specular has zero light sampling probability). Dome light pdf is also factored in if the ray missed (dome light hit via BSDF sampling).

### ptDiffuseAlbedo Output

Alongside `pathColor`, the shader computes `ptDiffuseAlbedo` — a denoiser input for first-bounce diffuse albedo. When the first bounce is specular with path splitting enabled, it looks through to the second hit's base color or emission, modulated by the first bounce's specular tint. It also implicitly captures volume absorption and other effects accumulated in `pathWeight` up to that point.

---

## RC_UPDATE Mode

When compiled with `RC_UPDATE`, the shader runs at reduced resolution (1/RC_UPDATE_SCALE of render width and height). Key differences:

- Paths are capped at `RC_MAX_PATH_DEPTH` (6).
- At each non-specular bounce, the hit position is inserted into the RC hash table, and a throughput weight is tracked from that vertex onward.
- Every time radiance is computed (emission, direct lighting, dome light miss), it is written to the RC accumulation buffer for *all* recorded vertices, scaled by their respective throughputs.
- Path splitting and `ptDiffuseAlbedo` are disabled.

---

## Voxel-Mode-Only Features

The following are gated on `sceneParams.voxelMode == 1` and return zero/noop in glTF mode:

- **Dome light** — `getDomeLightColor()` returns black in non-voxel mode. This means all environment lighting (sun, sky gradient, ground color) is voxel-only.
- **Dome light direct sampling** — the entire dome light MIS branch is skipped in non-voxel mode.
- **`domeLightPdf()`** — returns 0 in non-voxel mode, so BSDF-hit MIS against the dome light has no effect.
- **Chunk debug coloring** — tints the primary hit based on chunk coordinates.
- **Water absorption** — `computeSegmentAbsorption` and `computePassthroughAbsorption` use `voxelBoundsMin/Max_WS` to compute water travel distance, which is only populated in voxel mode. The `PerTriData` flag `TRIANGLE_FLAG_IS_WATER` is also never set outside voxel mode, so water-surface detection itself is voxel-only.

---

## Hit Groups

Three hit groups exist in the path tracing pipeline:

- **`PT_HITGROUP_PRIMARY`** — used for BSDF-sampled rays. Full closest-hit that records the complete hit, plus anyhit for alpha cutout and passthrough.
- **`PT_HITGROUP_LIGHTS`** — used for direct light shadow rays. Lightweight closest-hit that only records instance ID, triangle index, hit position, and UV (enough to verify we hit the intended light triangle). Uses the same anyhit as primary for passthrough.
- **`PT_HITGROUP_DOME_LIGHT`** — used for dome light shadow rays. Minimal closest-hit that just sets `PAYLOAD_FLAG_DID_HIT`. Miss = dome light was reached. Uses the same anyhit as primary for passthrough.
