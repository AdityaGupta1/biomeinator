_Last edited: 2026-04-26_

# HLSL Utility Libraries

`.hlsli` headers under `src/shaders/`. This entry covers purpose and non-obvious gotchas.

- **`common/global_params.hlsli`** — single `GlobalParams` cbuffer that every shader reads.
  All params and `HeapIndices` for bindless access live here.
- **`common/path_tracing_common.hlsli`** — shared by G-buffer, path tracing, and light
  sampling. Declares TLAS, geometry buffers, and the hit shaders (`AnyHit`,
  `ClosestHit_Primary`). See [path_tracing.md](path_tracing.md).
- **`common/payload.hlsli`** — `Payload` struct passed through `TraceRay`.
- **`materials/materials.hlsli`** — BSDF evaluation and sampling, bindless texture reads.
- **`materials/volume.hlsli`** — water absorption and underwater logic. **Include order
  dependency**: must be included after `path_tracing_common.hlsli` because it references
  `instanceDatas` and `perTriDatas` without declaring them.
- **`light/ris.hlsli`** — Resampled Importance Sampling for direct area light sampling.
- **`light/dome_light.hlsli`** — dome light (sun + sky gradient), voxel mode only.
- **`light/light_sampling.hlsli`** — shared helpers for sampling points on area light
  triangles and computing solid-angle PDFs.
- **`util/rng.hlsli`** — PCG-based hash RNG. Sequential state — call order within a shader
  matters for reproducibility.
- **`util/sampling.hlsli`** — cosine-weighted hemisphere and spherical cap sampling.
- **`util/math.hlsli`** — TBN construction, cosTheta, coordinate helpers.
- **`util/color.hlsli`** — luminance, sRGB conversion, DLSS specular albedo helper.
- **`util/ray.hlsli`** — ray evaluation and ray cone helpers.
- **`util/packing.hlsli`** — octahedral normal encoding/decoding.
