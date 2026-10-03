_Last edited: 2026-10-02_

# Mesh Generation

`Chunk::createInstances()` in `src/terrain/chunk.cpp` — converts block data into vertex/index buffers. Per-face emission with segment culling (not traditional greedy meshing despite the filename).

## Three Instances Per Chunk

Terrain and water are separate `Instance` objects with independent BLAS. Water gets `FACE_FLAG_IS_WATER` on all triangles so the path tracer can handle it differently. If no water faces are generated, the water instance is freed in `cleanUnusedInstances`. A third, the waterline instance, holds only the split side faces described below. Few chunks have any, so unlike the other two it is not requested up front: the worker meshes the bands into chunk-owned vectors, and `createWaterlineInstance` makes the instance on the main thread when the chunk is queued for its BLAS, only if there are bands.

## Face Media

Every cube face of a water or volume block records the medium in front of it (the neighbor's) and
behind it (its own) in its face flags. The front is air where a partial-height block leaves the
rest of a cell open: a water top's own top face, and a bottom face resting on a water top. Faces of
air-medium blocks (solid, cutout, foliage) record no boundary even when they border water: light
never crosses an opaque face, and a diffuse sample dipping below a normal-mapped seabed's shading
normal would otherwise flip the path out of the water. X-shaped and custom models record none
either, so foliage standing in water leaves the path's medium alone.

## Waterline Bands

A volume block's side face next to a water top borders water below the surface and air above it,
so it is emitted as two bands split at the surface height, each with its own front medium. The
bands go into the waterline instance rather than the terrain one because they must move with the
waves: the displacement pass moves every vertex resting at the 7/8 surface height, so the split
vertices follow `waveHeight` exactly like the water surface's edge vertices at the same position
and the joint stays watertight every frame. The terrain instance cannot carry them (packed
vertices, never refit) and neither can the water instance (its material is untextured; the
waterline instance uses the terrain material, so the usual per-face overrides apply).

The bands' v coordinate counts down from the cell's top edge, as on a whole side face, and the
displacement pass rewrites it on moved side-face vertices. v is linear in height on a vertical
face, so the texture stays fixed in world space and only the split line slides over it. The bands
carry no `FACE_FLAG_IS_WATER_TOP`, so the G-buffer reports camera-only motion for them, which is
correct for a world-locked texture.

**Unresolved:** waterline bands are never registered as area lights, so an emissive volume block
next to a water top would light the world through its bands by BSDF sampling only. Registering them
is not just a matter of adding them: per-frame displacement never rebuilds the light sampling
structure, so their triangle areas (and NEE's light pdf) would go stale. No emissive volume block
exists yet; the fix is left open.

## Crack Prevention

All vertex positions are integer-derived (block position + vertex offset from lookup tables). Combined with integer `TransformOffset` per chunk, adjacent chunk meshes share exact vertex positions at boundaries — no floating-point seams.

## X-Shaped Block Jitter

Blocks opt into ±0.2 tangent-plane jitter with the `randomJitter` JSON flag.
X-shaped flora uses its world XZ hash so vertically stacked plant segments remain
aligned. Custom models include Y in their hash and rotate the jitter plane with the
attachment face, keeping wall and ceiling models seated on their supports.

## Texture Slice Indexing

Terrain textures are a `Texture2DArray` of 16×16 tiles (see [scene → materials_textures.md](../scene/materials_textures.md)). Per-vertex UVs are the corner offsets `{0,1}×{0,1}` directly — there is no atlas multiplier. The slice index lives in `PerFaceData.texArraySliceIdx`, written once per face during mesh gen.

**Slice ordering invariant:** slice indices are assigned by `Blocks::init()` in first-reference order over the block JSONs, stored pre-resolved in `BlockData::texSlices`, and `TerrainMaterials::init()` loads the arrays in that same order via `Blocks::getTextureNames()` — which is why `Blocks::init()` must run first. Untextured blocks (air, water) carry `TEX_SLICE_INVALID`; out-of-range lookups (biome tint, OMM cutout) return false, and water's material has no textures so the slice is never sampled.

## Emissive Triangle Tracking

Faces from `markAsEmitter` blocks record their triangle indices into a separate list, which feeds `Instance::addAreaLights()`. The path tracer then importance-samples these triangles as area light sources.

## Why Separate From Segments

Mesh generation (`GENERATING_GEOMETRY`) is a separate stage from segment classification (`GENERATING_SEGMENTS`) because the terrain manager needs to allocate `Instance` objects from the scene on the main thread before the worker can write into them. Segments run purely on workers.
