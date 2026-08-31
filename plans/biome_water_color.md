# Per-Biome Water Absorption Color

Goal: water color varies by biome — dark murky swamp water, light blue ocean — with soft,
world-stable color borders (a gentler version of the Meeting of the Waters near Manaus):
the border must sit at the same world position no matter the view angle or distance.
Some per-ray noise is acceptable; DLSS / temporal accumulation absorbs it. Investigated
2026-08-16 against the current codebase.

## Model

Water color comes entirely from Beer's law absorption — the water material's base color
is white (`terrain_materials.cpp:59`), so no material changes are needed. Today the
absorption coefficient is a single constant (`water.hlsli:11`,
`waterSigmaA = float3(0.35, 0.06, 0.02) * 0.4`). The plan replaces it with a world-XZ
field σₐ(x, z) and computes transmittance as the line integral

```
T = exp(-∫ σₐ(xz(t)) dt)   over the underwater span of each ray segment
```

Because the field is a property of the world and not of the ray, borders are
view-independent by construction. Looking down at a water surface, the refracted path
descends steeply so its XZ barely moves — each surface point shows the water color of
the biome directly beneath it.

### σₐ field: second slice of the existing biome map

The biome map (`src/rendering/biome_map.cpp`) is already exactly the right vehicle: a
low-res camera-centered world-XZ texture of per-biome colors (currently grass tint),
deterministically refilled from seed and sampled bicubically. Water uses a second slice
holding a per-biome water color. The bicubic B-spline smoothing (borders ~2 texels ≈ 16
blocks wide) is intentionally kept for the water slice — soft borders are the desired
look, not a Manaus-sharp line.

### Encoding: sRGB color at a reference distance

Don't store σₐ directly in UNORM8 — the current values (0.14 / 0.024 / 0.008) would
quantize to mush. Store an artist-friendly "water color at reference distance D" in the
sRGB texture (matching the `colorFromHex` authoring style of `grassTint`) and derive in
the shader:

```
sigmaA = -log(max(color, eps)) / WATER_COLOR_REF_DISTANCE
```

with D ≈ 5–10 blocks (a `common_settings.h` define). sRGB puts its precision at the dark
end, which is exactly where σₐ needs it, and hardware sRGB decode happens before
filtering so interpolation is in linear color space.

### Integration: stratified jittered sampling, one exp per span

- Sample count: `N = clamp(ceil(L / 8), 1, 8)` — 1 sample for spans ≤ 8 blocks, 8 for
  spans ≥ 64. Below the cap this is a constant 8-block stratum width, deliberately equal
  to `BIOME_MAP_BLOCKS_PER_TEXEL` (`common_settings.h:51`), so sampling density matches
  the field's resolution.
- Each sample is jittered uniformly within its stratum (N = 1 degenerates to one uniform
  sample over the whole span). This is standard stratified sampling: the optical depth
  estimate `τ = Σ σ(tᵢ) · L/N` is unbiased and banding is impossible by construction;
  residual error is per-ray noise for DLSS to eat. Past the 8-sample cap, widening
  strata alias into noise instead of banding for the same reason.
- Uniform symmetric strata preserve reciprocity: a span traversed A→B and B→A computes
  the same expected transmittance, keeping view rays and shadow rays through the same
  water consistent. (This is why the spacing is linear, not exponential — every unit of
  optical depth darkens equally regardless of where it sits along the span, and the
  smooth field has no near-end weighting to exploit.)
- Accumulate τ as a float3 across samples and exponentiate **once** per span:
  `Π exp(-σᵢΔt) = exp(-Σ σᵢΔt)`. Per-sample work is one map fetch and one fma.
- Known footnote: temporal averaging of `exp(-τ)` converges to `E[exp(-τ)]`, which by
  Jensen is marginally brighter than `exp(-E[τ])` where σ varies along the span. With
  this smooth a field it is far below visibility — noted so it isn't a surprise.

## Why the biome map, not per-vertex data or the payload

- Greedy meshing merges water into large quads spanning biome borders; per-vertex biome
  color would either split quads or interpolate across huge quads.
- Rays that *start* underwater (submerged camera, bounce segments) never hit a water
  front face, so neither vertex data at a hit nor a σ recorded in the AnyHit can color
  them. The map is addressable by XZ from anywhere.
- Carrying σ in the payload would grow the DXR payload (perf-sensitive) and has the same
  underwater-start blind spot.

## Facts verified against the code (what makes this small)

- All absorption funnels through `computeWaterAbsorption` in `water.hlsli`, via exactly
  two wrappers with four call sites:
  - `computeSegmentAbsorption` (`water.hlsli:58`): `path_tracing.rgs.hlsl:118` (primary
    segment) and `:493` (bounce segments).
  - `computePassthroughAbsorption` (`water.hlsli:68`): `light_sampling.hlsli:139` and
    `dome_light.hlsli:163`. Both call sites have the ray in hand for the new
    origin/direction parameters.
- The G-buffer pass initializes the water payload fields (`gbuffer.rgs.hlsl:117`–`120`)
  but never applies absorption — no changes there.
- The biome map is RGBA8 sRGB with an unused alpha channel, filled by
  `ChunkGenerator::fillBiomeRect` — a pure function of world seed (the scroll logic in
  `biome_map.cpp` depends on regenerated strips matching the overlap exactly), so water
  in unloaded chunks is colored correctly and the border speckle from per-column biome
  jitter is anchored to world texels, not the camera.
- The biome map sampler is linear-clamp (`common_registers.h:31`), so spans past the map
  edge clamp to the border biome's color — graceful.
- `getBiomeTint` (`biome_map.hlsli:44`) guards on `sceneParams.biomeMapTexelsPerSide == 0`;
  `BiomeMap::update` runs only in voxel mode (`renderer.cpp:580`). The water path needs
  the same guard, falling back to the current constant.
- The payload already carries a `RandomNumberGenerator` (used for alpha cutout and fog
  in-scatter) — the source for jitter draws. Both wrappers currently take
  `const Payload`, so the rng must be threaded `inout` or the jitter would correlate
  with later draws in the path.
- The single entry/exit span approximation for multiple water bodies along one ray
  (`water.hlsli:73`) is pre-existing and unchanged; one biome-integrated span adds no
  new failure mode to it.

## Implementation

### 1. Per-biome color (CPU)

- `biome.h`: add `glm::vec3 waterColor` (sRGB) to `BiomeData` next to `grassTint`.
- `biome.cpp`: set per-biome hex values — e.g. clear blue for OCEAN, murky green-brown
  for swamp; the current constant corresponds to roughly `exp(-waterSigmaA * D)` as a
  starting point for the others. Tuning happens after the plumbing lands.
- `common_settings.h`: `#define WATER_COLOR_REF_DISTANCE` (shared with HLSL).

### 2. Biome map second slice (`biome_map.cpp`)

- Texture becomes a 2-slice `Texture2DArray` (`DepthOrArraySize = 2`, SRV
  `D3D12_SRV_DIMENSION_TEXTURE2DARRAY`): slice 0 = grass tint, slice 1 = water color.
- The CPU `biomes` array, scroll logic, and origin/texels scene params are shared
  unchanged. The upload loop writes both slices; upload buffer doubles (still tiny);
  one `CopyTextureRegion` per subresource.

### 3. Shader sampling (`biome_map.hlsli`)

- `sampleBiomeMapBicubic` gains a slice parameter; `Texture2D` → `Texture2DArray`.
  `getBiomeTint` passes slice 0.
- New `sampleWaterSigmaA(float2 posXZ_WS)`: bicubic sample of slice 1, then
  `-log(max(color, eps)) / WATER_COLOR_REF_DISTANCE`. Returns the constant
  `waterSigmaA` when `biomeMapTexelsPerSide == 0` (non-voxel mode).

### 4. Integration (`water.hlsli`)

- `computeWaterAbsorption(float3 startPos_WS, float3 dir, float spanLength,
  inout RandomNumberGenerator rng)`: N per the ramp above; loop strata drawing one
  jitter each; accumulate `tau += sampleWaterSigmaA(xz(tᵢ)) * (spanLength / N)`; return
  `exp(-tau)`.
- `computeSegmentAbsorption`: already has origin/dir; span = `getSegmentVolumeDistance`;
  thread the rng through.
- `computePassthroughAbsorption`: gains `rayOrigin`/`rayDir` (+ rng); span start at
  `waterEntryT`, length `max(min(waterExitT, rayEndT) - waterEntryT, 0)` — strata laid
  over the water span only, not the full ray.
- Update the four call sites accordingly (`light_sampling` / `dome_light` pass the
  caller's ray and rng).

## Interactions checked — no changes needed

- **Fog**: `applySegmentFog` skips underwater segments (`path_tracing.rgs.hlsl:81`);
  fully independent.
- **Orphan water backface handling** (`path_tracing.rgs.hlsl:57`): keys off payload
  flags and entry T only; unaffected.
- **DLSS guide buffers**: the G-buffer pass applies no absorption, so first-hit albedo
  guides are unchanged.

## Testing

- A world where all biomes get the same `waterColor` (matching the old constant at the
  reference distance) should look identical to current output modulo jitter noise —
  bit-identical is not achievable since the rng draw count changes for water paths.
- Goldens: every water scene changes (e.g. `water_reflection_diffuse_albedo`). Land the
  plumbing, tune colors visually, regenerate goldens in one commit; consider a dedicated
  golden framing a swamp/ocean border.
- Visual checks: fly around above a swamp/ocean water border — the blend line must stay
  fixed in world space at all view angles and distances; cross the border while
  submerged (color should blend in proportion to distance traveled through each biome);
  low sun casting shadow rays across border water; camera underwater in swamp vs ocean.
