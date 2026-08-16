# Opacity Micromaps for Voxel Alpha Cutout

Goal: skip the anyhit shader entirely for alpha-tested terrain in voxel mode using DXR 1.2
Opacity Micromaps (OMMs). Investigated 2026-08-16 against the vendored DirectX-Specs
`Raytracing.md` (§ "Opacity micromaps") and the current codebase.

## Why this is a near-ideal fit

- **Every terrain hit runs anyhit today, including plain stone/dirt.** Opacity is
  per-geometry pre-OMM, each chunk's terrain instance is one geometry mixing opaque and
  cutout faces, and `AnyHit`'s `testAlphaCutout` is true whenever the material has a diffuse
  texture — which the terrain DEFAULT material always does. So the BLAS is non-opaque and
  *every* hit invokes anyhit + a texture sample just to conclude alpha == 1. OMM special
  index `FULLY_OPAQUE (-2)` removes that for opaque tiles; real OMMs remove it for cutout
  tiles.
- **All cutout alpha is strictly binary at every mip.** Verified against `diffuse.png`: 25
  of 1024 tiles have transparency, and every texel in them is exactly 0 or 255. The CPU mip
  generator preserves this: `quantizeAlphaToCoverageTile` re-binarizes each mip after the
  box filter (top-ranked texels → 255 up to the source coverage fraction, rest → 0). So
  2-state OMMs (`OC1_2_STATE`) suffice — no "unknown" micro-triangles, meaning anyhit is
  *never* invoked for terrain, not just rarely. It also means the anyhit's stochastic
  branch (`rng.nextFloat() > a`) and `trySplitMaterial`'s alpha split never actually fire
  for terrain today — sampled alpha is always exactly 0 or 1 — so terrain anyhit is pure
  overhead with zero stochastic behavior to preserve.
- **The OMM encoding is exact, not approximate.** Every face (cube and X-shaped) uses the
  same corner UVs `{0,1}×{0,1}` with the same vertex order and the same `(0,1,2),(0,2,3)`
  triangle split (`chunk.cpp` `uvOffsets` / index emission). A subdivision-level-4 OMM puts
  a 16×16 barycentric grid on the triangle; each micro-triangle falls entirely inside
  exactly one texel of the 16×16 tile. Combined with the point sampler in voxel mode
  (`MIN_MAG_MIP_POINT`), the OMM reproduces the mip-0 alpha test bit-exactly at close range.
- **Massive sharing.** OMMs live in an OMM Array separate from BLASes and are referenced by
  index, reusable across all BLASes. Two triangles per quad × 25 cutout slices =
  **50 OMMs total, 32 bytes each (~1.6 KB), built once at startup**, covering every chunk
  in the world.

## API availability

- The vendored headers and Agility SDK are already OMM-capable at the API level: Agility
  SDK **1.616** (exactly what `main.cpp` exports) is the retail release that shipped OMMs;
  `external/DirectX-Headers` has all the `D3D12_RAYTRACING_OPACITY_MICROMAP_*` types.
- Support is gated on `D3D12_RAYTRACING_TIER_1_2` (`D3D12_FEATURE_DATA_D3D12_OPTIONS5.
  RaytracingTier`). There is no tier check in `renderer_init.cpp` today; one must be added,
  with the current anyhit path as fallback (it stays intact anyway — see below).
- Hardware: NVIDIA supports DXR 1.2 OMMs on all RTX GPUs via driver (hardware-accelerated
  micro-triangle culling on Ada/Blackwell RT cores). AMD does not support OMM through
  RDNA4; Intel plans it for Xe3. So the fallback path matters for portability but the
  primary target (NVIDIA, given NVAPI SER is already required) is covered.

## The one wrinkle: the fog RayQuery needs SM 6.9

The spec is explicit that if traversal encounters an OMM-linked triangle and the traversal
didn't opt in, **behavior is undefined**. Opt-in is per-mechanism:

- `TraceRay` path: `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS` on
  `D3D12_RAYTRACING_PIPELINE_CONFIG1` — pure API-side, zero shader changes, works with the
  current SM 6.6 shaders. One line in `pipeline_builder.h` (`makeRtPipeline` is the single
  creation path for all RT PSOs).
- Inline `RayQuery` (the fog sun-occlusion query in `fog.hlsli`): opt-in is a second
  template parameter `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`, which **requires Shader Model
  6.9**. The project compiles at `SHADER_VERSION 6_6`. SM 6.9 went retail in Agility SDK
  **1.619** with DXC **1.9.2602.16** (Feb 2026).

So enabling OMMs correctly means upgrading the vendored Agility SDK 1.616 → 1.619 and DXC,
and bumping `SHADER_VERSION` to `6_9` (at least for shaders including `fog.hlsli`, i.e. the
path tracing shaders — in practice bump globally). In exchange:

- The fog query's manual alpha test (deterministic 0.5 threshold, mip 0) is *exactly* what
  the OMM resolves in hardware, so the candidate loop only remains to ignore water
  (glossy-transmissive) candidates.
- SM 6.9 retail also brings native `MaybeReorderThread` — a future follow-up could drop the
  NVAPI SER dependency.
- Caveat: shipping SM 6.9 DXIL requires driver support; if the driver lacks it,
  `CreateStateObject` fails outright. If that's a concern, compile the fog query in two
  variants (the NRC-style variant infrastructure already exists) and pick at PSO creation
  based on the tier check.

## Implementation sketch

1. **Bake** (`terrain_materials`): at texture load, find slices with any alpha < 255
   (assert all texels are 0/255), assign each a compact cutout index. For each, bake two
   level-4 `OC1_2_STATE` bitmasks (lower/upper quad triangle): for each of the 256
   micro-triangles, map its index along the spec's space-filling curve to its barycentric
   centroid → UV → texel, state = alpha != 0. (Micro-tri ↔ texel alignment is exact, so
   centroid sampling is conservative-free.)
2. **OMM Array build**: one `BuildRaytracingAccelerationStructure` with
   `Type = OPACITY_MICROMAP_ARRAY`, histogram `[{50, level 4, OC1_2_STATE}]`. Result
   sub-allocated from `sharedAcsBuffer` (spec allows intermixing with AS; 128-byte
   alignment; buffer already lives permanently in the AS state). Needs a UAV barrier
   before the first BLAS build that references it — same pattern as the existing BLAS→TLAS
   barrier in `makeTlas`.
3. **Mesh gen** (`chunk.cpp`): emit a per-triangle OMM index alongside
   `host_perTriDatas`: `FULLY_OPAQUE (-2)` for faces of opaque slices,
   `2 * cutoutIdx + triInQuad` for cutout faces. `DXGI_FORMAT_R16_UINT` is safe (verify
   special-index encoding in R8 before trying 1 byte/tri); ~2 bytes/tri ≈ 16 KB per full
   chunk, small next to `PerTriangleData`. Chunks with zero cutout faces can skip the
   index buffer and stay plain `TRIANGLES` geometry.
4. **BLAS build** (`acs_helper`): optional OMM linkage on `BlasBuildInputs`; when present,
   geometry type becomes `OMM_TRIANGLES` with
   `D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC { pTriangles, pOmmLinkage }`, linkage =
   index buffer VA/stride/format + OMM Array VA. Index buffer must be in
   `NON_PIXEL_SHADER_RESOURCE` state at build. Terrain BLASes are always rebuilt, never
   refit, so `ALLOW_OMM_LINKAGE_UPDATE` is not needed; the water BLAS keeps no OMM linkage
   so its refit path is untouched.
5. **Pipeline**: add `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS` in
   `makeRtPipeline` when OMMs are active (voxel mode + tier 1.2). Spec notes a small cost
   to setting it when unused, so gate it.
6. **Fog query**: add `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`; drop the terrain alpha-test
   branch from the candidate loop (water ignore stays).
7. **Shaders otherwise unchanged.** `AnyHit` stays as-is: it still serves water refraction
   passthrough (water has no OMM), glTF mode (linear sampler, fractional alpha), and the
   non-tier-1.2 fallback. With 2-state OMMs linked, terrain triangles simply never invoke
   it. `NO_DUPLICATE_ANYHIT_INVOCATION` stays for the same reasons.

## Behavior changes (goldens will move)

- **Close range: bit-identical.** OMM at level 4 == point-sampled mip-0 alpha test.
- **Distance (mips 1–4)**: alpha is binary at every mip (coverage-preserving
  re-binarization in `quantizeAlphaToCoverageTile`), so the current behavior is already a
  deterministic cutout test — just against a *coarser* binary pattern selected by the ray
  cone. OMMs have no LOD: traversal always tests the mip-0 pattern. Same coverage, but
  higher-frequency detail at distance, which is exactly what the alpha mip quantization
  exists to smooth over — expect some added distant-foliage shimmer/aliasing for the
  denoiser/DLSS to absorb. (If it's objectionable, a 4-state OMM with "unknown" where the
  mips disagree could fall back to anyhit at silhouettes only, but start with plain 2-state
  from mip 0 — it's the geometric ground truth, and shadow/GI rays get exact cutout.)
- Foliage-region goldens need regolding; PIX can visualize OMMs for debugging.

## Expected wins

- Anyhit drops to zero for all terrain geometry — including the ~9%-of-frame shadow rays,
  which currently can't terminate in hardware on any terrain triangle because the whole
  geometry is non-opaque. Traversal for terrain becomes fully fixed-function.
- Microsoft cites up to ~2.3× RT throughput in alpha-heavy scenes; realistic expectation
  here is a solid double-digit % on foliage-heavy views. The win is purely performance —
  there is no stochastic alpha noise today to reduce (see above).
