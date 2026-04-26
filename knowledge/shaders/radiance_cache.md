_Last edited: 2026-04-26_

# Neural Radiance Cache (NRC)

Replaces the former custom hash-grid radiance cache. Uses NVIDIA's NRC SDK
(`NRC_D3D12.dll`) which trains a small neural network each frame to predict
indirect radiance given position + direction.

## Shader Variants

Three variants of `path_tracing/path_tracing.rgs.hlsl` are compiled:

| Variant | Define | Purpose |
|---|---|---|
| `path_tracing/path_tracing.rgs.hlsl` | (none) | Plain path tracer, no cache. Used when NRC is disabled. |
| `nrc/nrc_update.rgs.hlsl` | `NRC_UPDATE 1` | Training pass at reduced resolution (`trainingDimensions`). Writes path vertices + radiance for the neural network to learn from. |
| `nrc/nrc_query.rgs.hlsl` | `NRC_QUERY 1` | Query pass at full `frameDimensions`. Paths terminate early when NRC determines it can predict the remaining radiance, writing a query point for later resolve. |

The defines gate NRC-specific code in `path_tracing/path_tracing.rgs.hlsl`: `NrcUpdateOnHit`,
`NrcUpdateOnMiss`, `NrcSetBrdfPdf`, `NrcCanUseRussianRoulette`,
`NrcWriteFinalPathInfo`.

## NRC Buffers

Five SDK-owned buffers are bound as UAVs to the path tracing root signature:
`QueryPathInfo`, `TrainingPathInfo`, `TrainingPathVertices`,
`QueryRadianceParams`, `CountersData`. These are obtained from
`nrcContext->GetBuffers()` each frame.

## Custom Resolve

NRC's built-in `Resolve()` expects a 2D texture output, but the path tracer
writes to a structured buffer (`dev_pathTracingRawBuffer`). A custom resolve
compute shader (`nrc/nrc_resolve.cs.hlsl`) reads `QueryPathInfo` and
`QueryRadiance`, multiplies by the path's prefix throughput, and adds to the
raw buffer. This also handles path splitting — linear indexing at
`frameDimensions` matches the interleaved buffer layout.

The built-in `Resolve()` is used only for debug visualization modes, with a
temporary `RWTexture2D<float4>` debug texture.

## Path Splitting

When `doPathSplitting` is on, `frameDimensions` doubles in width to match the
doubled dispatch. Each split thread creates its own NRC query. The update pass
is unaffected — it always runs at `trainingDimensions`.

## NRC Pixel Indexing

NRC context creation uses raw `DispatchRaysIndex().xy`, not `getPixelIdx()`.
Normal shading still uses `getPixelIdx()` for path-split pixel semantics.

## Runtime Toggle

Toggling NRC on creates the context + configures it. Toggling off calls
`flush()` + `Destroy`. On resize or `doPathSplitting` change, the context is
destroyed and recreated (not just reconfigured) to avoid crashes with debug
resolve modes.

## Configuration

`NrcConstants` lives in a separate root CBV (register space 5, b0), outside
the main `GlobalParams` cbuffer. `BeginFrame` and `PopulateShaderConstants`
are called each frame before NRC dispatches. Scene bounds come from
`voxelBoundsMin/Max_WS` in voxel mode and from `Scene`'s loaded glTF
world-space bounds in glTF mode. The broad fallback cube is only used if a
glTF scene has no recorded bounds.
