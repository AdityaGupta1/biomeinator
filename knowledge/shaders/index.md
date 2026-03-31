_Last edited: 2026-03-30_

# Shaders Knowledgebase

HLSL ray tracing and compute shaders, shared utility libraries, and the build-time compilation pipeline.

| Entry | Description |
|---|---|
| [path_tracing.md](path_tracing.md) | Main path tracer: MIS/RIS, path splitting, radiance cache integration |
| [gbuffer.md](gbuffer.md) | G-buffer ray generation shader, DLSS input outputs |
| [radiance_cache.md](radiance_cache.md) | rc_update / rc_resolve / rc_evict shaders and hash-probe scheme |
| [collect.md](collect.md) | Temporal accumulation compute shader, tonemapping (AGX, Khronos) |
| [common_structs.md](common_structs.md) | CPU/GPU shared structs, params, registers, enums, hit groups |
| [hlsli_libraries.md](hlsli_libraries.md) | Utility .hlsli headers: math, rng, sampling, material, ris, dome |
| [shader_compilation.md](shader_compilation.md) | dxc build-time compilation, .hlsl → .fxh embedding, shader types |
