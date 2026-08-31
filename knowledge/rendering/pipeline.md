_Last edited: 2026-08-30_

# DXR Pipeline

`pipeline_builder.h` provides `makeRtPipeline()` which creates a DXR state object, builds
the shader table, and fills a `D3D12_DISPATCH_RAYS_DESC`.

All pipelines use `PIPELINE_CONFIG1` with `SKIP_PROCEDURAL_PRIMITIVES` (all geometry is
triangles) — measured ~2% frame time (2026-08).

Root-signature gotcha: any shader that uses NVAPI intrinsics (e.g. `NvReorderThread`) needs
the fake extension UAV (u1738/space1738) bound in that pipeline's root signature or
`CreateStateObject` fails with E_INVALIDARG. Currently only the path tracing root signature
has it.

## Single Library Per Pipeline

Each RT pipeline uses one DXIL library containing the ray generation, miss, and all hit
shaders. This works because each `.rgs.hlsl` file includes all its hit/miss shaders via
`path_tracing_common.hlsli`. There's no multi-library composition.

## MaxTraceRecursionDepth = 1

All pipelines set `MaxTraceRecursionDepth = 1`. Recursive ray tracing is not used — the
path tracer uses an iterative bounce loop with explicit `TraceRay` calls. This keeps stack
usage predictable.

## Root Signatures

Each pass (gbuffer, path tracing, collect, postprocess, debug view) has its own
root signature. They all use `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` for bindless descriptor
heap access. Root parameters are inline CBVs/SRVs/UAVs (not descriptor tables) — resources
are bound by GPU virtual address.
