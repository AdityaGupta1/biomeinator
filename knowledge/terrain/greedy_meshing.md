_Last edited: 2026-04-26_

# Mesh Generation

`Chunk::createInstances()` in `src/terrain/chunk.cpp` — converts block data into vertex/index buffers. Per-face emission with segment culling (not traditional greedy meshing despite the filename).

## Two Instances Per Chunk

Terrain and water are separate `Instance` objects with independent BLAS. Water gets `TRIANGLE_FLAG_IS_WATER` on all triangles so the path tracer can handle it differently. If no water faces are generated, the water instance is freed in `cleanUnusedInstances`.

## Crack Prevention

All vertex positions are integer-derived (block position + vertex offset from lookup tables). Combined with integer `TransformOffset` per chunk, adjacent chunk meshes share exact vertex positions at boundaries — no floating-point seams.

## X-Shaped Block Jitter

Each X-shaped block (grass, flowers) gets ±0.2 XZ jitter from a per-chunk RNG. This breaks the grid alignment that would otherwise be very visually obvious in fields of grass.

## Emissive Triangle Tracking

Faces from `emitsLight` blocks record their triangle indices into a separate list, which feeds `Instance::addAreaLights()`. The path tracer then importance-samples these triangles as area light sources.

## Why Separate From Segments

Mesh generation (`GENERATING_GEOMETRY`) is a separate stage from segment classification (`GENERATING_SEGMENTS`) because the terrain manager needs to allocate `Instance` objects from the scene on the main thread before the worker can write into them. Segments run purely on workers.
