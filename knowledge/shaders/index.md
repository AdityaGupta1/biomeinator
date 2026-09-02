_Last edited: 2026-09-02_

# Shaders Knowledgebase

HLSL ray tracing and compute shaders, shared utility libraries, and the build-time compilation pipeline.

Shaders are organized into subdirectories under `src/shaders/`: `common/` (shared headers), `light/` (lighting), `materials/` (material evaluation, mipmap, water), `path_tracing/` (main path tracer, G-buffer, collect), `postprocess/` (tonemapping, debug view), and `util/` (math, RNG, sampling). All cross-directory includes use root-relative paths via a `-I` flag pointing at the shader root.

| Entry | Description |
|---|---|
| [path_tracing.md](path_tracing.md) | Main path tracer: MIS/RTSL, path splitting |
| [materials.md](materials.md) | Material lobes, the two Fresnel conventions, rough glass, energy compensation, shading normal |
| [gbuffer.md](gbuffer.md) | G-buffer ray generation shader, DLSS input outputs |
| [collect.md](collect.md) | Temporal accumulation compute shader, tonemapping (AGX, Khronos) |
| [common_structs.md](common_structs.md) | CPU/GPU shared structs, params, registers, enums, hit groups |
| [hlsli_libraries.md](hlsli_libraries.md) | Utility .hlsli headers: math, rng, sampling, material, dome |
| [shader_compilation.md](shader_compilation.md) | dxc build-time compilation, .hlsl → .fxh embedding, shader types |
