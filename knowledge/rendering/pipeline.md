_Last edited: 2026-04-26_

# DXR Pipeline

`pipeline_builder.h` provides `makeRtPipeline()` which creates a DXR state object, builds
the shader table, and fills a `D3D12_DISPATCH_RAYS_DESC`.

## Single Library Per Pipeline

Each RT pipeline uses one DXIL library containing the ray generation, miss, and all hit
shaders. This works because each `.rgs.hlsl` file includes all its hit/miss shaders via
`path_tracing_common.hlsli`. There's no multi-library composition.

## MaxTraceRecursionDepth = 1

All pipelines set `MaxTraceRecursionDepth = 1`. Recursive ray tracing is not used — the
path tracer uses an iterative bounce loop with explicit `TraceRay` calls. This keeps stack
usage predictable.

## Root Signatures

Each pass (gbuffer, path tracing, collect, NRC resolve, postprocess, debug view) has its own
root signature. They all use `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` for bindless descriptor
heap access. Root parameters are inline CBVs/SRVs/UAVs (not descriptor tables) — resources
are bound by GPU virtual address.
