_Last edited: 2026-09-17_

# HLSL Utility Libraries

`.hlsli` headers under `src/shaders/`. This entry covers purpose and non-obvious gotchas.

- **`common/global_params.hlsli`** — single `GlobalParams` cbuffer that every shader reads.
  All params and `HeapIndices` for bindless access live here.
- **`common/path_tracing_common.hlsli`** — shared by G-buffer, path tracing, and light
  sampling. Declares TLAS, geometry buffers, and the hit shaders (`AnyHit`,
  `ClosestHit_Primary`). See [path_tracing.md](path_tracing.md).
- **`common/payload.hlsli`** — `Payload` struct passed through `TraceRay`.
- **`materials/materials.hlsli`** — BSDF evaluation and sampling, bindless texture reads
  (design in [materials.md](materials.md)).
  All material color samplers take a `TexSampleCtx { mipLevel, arraySliceIdx }` rather
  than a bare mip. `sampleTexture` casts the bindless descriptor to `Texture2D` or
  `Texture2DArray` based on `material.hasArrayTexture()` — that flag must agree with
  the SRV dim set in `Scene::uploadPendingTextures` (see
  [scene → materials_textures.md](../scene/materials_textures.md)).
- **`materials/water.hlsli`** — water absorption and underwater logic. Included from
  `path_tracing_common.hlsli` so its helpers are available to `AnyHit` and every consumer
  of the common header (e.g. `dome_light.hlsli`, `light_sampling.hlsli`).
- **`sky/sky_lighting.hlsli`** — sun constants, sun direction and the sky/sun colour lookups
  shared by surfaces, fog and clouds. The sun direction is a closed-form function of
  `renderParams.animTime` rather than integrated state, so scrubbing time in either direction
  always reproduces the same sky. Lives outside `light/` because the sky LUT passes and the
  cloud code need it without pulling in the dome light's NEE machinery.
- **`light/dome_light.hlsli`** — dome light NEE and PDFs (sun cap sampling), voxel mode only.
- **`light/fog_density.hlsli`** — closed-form fog density profile and segment optical depth,
  split out so cloud lighting can attenuate by fog without depending on the fog march.
- **`light/fog.hlsli`** — the fog in-scattering march (god rays) on top of `fog_density.hlsli`.
- **`sky/cloud_occupancy.hlsli`**, **`sky/cloud_traversal.hlsli`**, **`sky/clouds.hlsli`** —
  block clouds, layered as cell occupancy → DDA intervals → transport. See
  [rendering → clouds.md](../rendering/clouds.md).
- **`light/light_sampling.hlsli`** — shared helpers for sampling points on area light
  triangles and computing solid-angle PDFs.
- **`util/rng.hlsli`** — PCG-based hash RNG. Sequential state — call order within a shader
  matters for reproducibility.
- **`util/sampling.hlsli`** — cosine-weighted hemisphere and spherical cap sampling.
- **`util/math.hlsli`** — TBN construction, cosTheta, coordinate helpers.
- **`util/ggx.hlsli`** / **`util/ggx_tables.hlsli`** — GGX distribution, Smith masking, VNDF
  sampling, refraction half vector/Jacobian, and the albedo tables (copied from Cycles,
  Apache-2.0) behind multiple-scattering compensation. Distribution functions take
  `alpha = roughness²`; table lookups take roughness.
- **`util/shading_normal.hlsli`** — Cycles' `ensure_valid_specular_reflection` port (Apache-2.0),
  applied in `ClosestHit_Primary` for materials with glossy lobes.
- **`util/color.hlsli`** — luminance, sRGB conversion, DLSS specular albedo helper.
- **`util/ray.hlsli`** — ray evaluation and ray cone helpers.
- **`util/packing.hlsli`** — octahedral normal encoding/decoding.
- **`util/FastNoiseLite.hlsli`** — vendored noise library
  ([FastNoiseLite](https://github.com/Auburn/FastNoiseLite), MIT). Kept as `.hlsli`,
  not upstream `.hlsl`, so the `*.hlsl` glob doesn't compile it as a standalone shader.
  Build `fnl_state` from compile-time constants so dxc prunes the unused noise-type branches.
