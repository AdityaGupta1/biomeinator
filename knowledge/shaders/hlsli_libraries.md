_Last edited: 2026-08-30_

# HLSL Utility Libraries

`.hlsli` headers under `src/shaders/`. This entry covers purpose and non-obvious gotchas.

- **`common/global_params.hlsli`** — single `GlobalParams` cbuffer that every shader reads.
  All params and `HeapIndices` for bindless access live here.
- **`common/path_tracing_common.hlsli`** — shared by G-buffer, path tracing, and light
  sampling. Declares TLAS, geometry buffers, and the hit shaders (`AnyHit`,
  `ClosestHit_Primary`). See [path_tracing.md](path_tracing.md).
- **`common/payload.hlsli`** — `Payload` struct passed through `TraceRay`.
- **`materials/materials.hlsli`** — BSDF evaluation and sampling, bindless texture reads.
  All material color samplers take a `TexSampleCtx { mipLevel, arraySliceIdx }` rather
  than a bare mip. `sampleTexture` casts the bindless descriptor to `Texture2D` or
  `Texture2DArray` based on `material.hasArrayTexture()` — that flag must agree with
  the SRV dim set in `Scene::uploadPendingTextures` (see
  [scene → materials_textures.md](../scene/materials_textures.md)).
- **`materials/water.hlsli`** — water absorption and underwater logic. Included from
  `path_tracing_common.hlsli` so its helpers are available to `AnyHit` and every consumer
  of the common header (e.g. `dome_light.hlsli`, `light_sampling.hlsli`).
- **`light/dome_light.hlsli`** — dome light (sun + sky gradient), voxel mode only. The sun
  direction is a closed-form function of `renderParams.animTime` rather than integrated state, so
  scrubbing time in either direction always reproduces the same sky.
- **`light/fog.hlsli`** — all air fog math: closed-form density profile and segment/to-sky
  optical depth, plus the in-scattering march (god rays). Needs the sun constants:
  **must be included after `dome_light.hlsli`**.
- **`light/light_sampling.hlsli`** — shared helpers for sampling points on area light
  triangles and computing solid-angle PDFs.
- **`util/rng.hlsli`** — PCG-based hash RNG. Sequential state — call order within a shader
  matters for reproducibility.
- **`util/sampling.hlsli`** — cosine-weighted hemisphere and spherical cap sampling.
- **`util/math.hlsli`** — TBN construction, cosTheta, coordinate helpers.
- **`util/color.hlsli`** — luminance, sRGB conversion, DLSS specular albedo helper.
- **`util/ray.hlsli`** — ray evaluation and ray cone helpers.
- **`util/packing.hlsli`** — octahedral normal encoding/decoding.
- **`util/FastNoiseLite.hlsli`** — vendored noise library
  ([FastNoiseLite](https://github.com/Auburn/FastNoiseLite), MIT). Kept as `.hlsli`,
  not upstream `.hlsl`, so the `*.hlsl` glob doesn't compile it as a standalone shader.
  Build `fnl_state` from compile-time constants so dxc prunes the unused noise-type branches.
