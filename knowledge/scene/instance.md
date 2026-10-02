_Last edited: 2026-10-02_

# Instance

`Instance` in `src/scene/scene.h/cpp` represents one ray-traceable object in the scene — a BLAS with associated metadata.

## Lifecycle

1. `Scene::requestNewInstance()` allocates on main thread, taking pooled CPU geometry vectors if any.
2. Worker thread fills `host_verts`, `host_idxs`, `host_perFaceDatas` directly (public vectors),
   and sets `trisPerFaceLog2` if a `PerFaceData` entry covers more than one triangle. Terrain
   also fills `host_packedTerrainVerts` and decodes `host_verts` back from it, so the BLAS
   build (from the staging upload) and the area lights use the same rounded geometry the shaders
   read; only the packed form goes resident. See
   [shaders → common_structs.md](../shaders/common_structs.md).
3. Worker calls `finalizeGeometry()` to mark data as ready.
4. Main thread calls `Scene::markInstanceReadyForBlasBuild()`.
5. `Scene::makeQueuedBlases()` uploads geometry to GPU, builds BLAS, writes `InstanceData`, then
   returns the `host_` vectors to the scene's pool.
6. On destruction, `Instance::reset()` frees all buffer sections and returns the ID to the pool.

## CPU Geometry Is Only Kept Until Upload (`hostGeometryPool`)

Nothing reads an instance's `host_` vectors after `makeQueuedBlases` copies them into the staging
buffers: refits, compaction, area lights and the TLAS all use the GPU copies. So an instance
returns its vectors, emptied but keeping their capacity, right after upload, and new instances
take them. Before this, every live instance kept its geometry and freed instances queued for
reuse kept theirs, about 2.8 MB per visible chunk. At render distance 30 that was about 10 GB of the
20 GB committed; with the pool the total is 10.2 GB.

The pool never frees anything while running, which is why it must stay bounded by other means.
Freeing these buffers stalled frames: trimming four sets from the pool took 20–40 ms on the main
thread, and up to 26 ms even with generation idle (see also [terrain → region_system.md](../terrain/region_system.md#freeing-without-stutter)).
Instead, Terrain only meshes a chunk while fewer than `maxInstancesHoldingHostGeometry` (512)
instances hold geometry, so the pool can never hold more sets than that. Streaming stays far
below the cap; only loads reach it, and there it adds about 1 s to a render distance 30 load,
which 1,024 removed at the cost of a pool twice as large. Water instances take full-size sets
from the pool despite holding little, so at present half the cap is spent on them.

Freed instances are destroyed in `freeInstance`, so it removes the instance from every list that
holds a raw pointer, `pendingTlasEntryAdds` included, before erasing it.

## Visibility

`setVisible(false)` excludes the instance from the TLAS without destroying its BLAS. This is how chunks outside render distance but within `createBlasDistance` are hidden — their geometry stays on the GPU but isn't traversed.

## Per-Face Data

`PerFaceData` is stored per mesh face, not per triangle: glTF instances have one entry per
triangle (`trisPerFaceLog2 == 0`), terrain and water one per quad (`== 1`), which halves the
buffer for terrain since both triangles of a quad always carried identical data. The shader
maps `PrimitiveIndex() >> trisPerFaceLog2` to the entry. Custom decorator models with an odd
triangle count get a degenerate padding triangle so every quad's first triangle stays even.

## Area Lights

`addAreaLights()` builds `AreaLight` structs from specified triangle indices. These store world-space vertex positions (transformed by the instance's float transform, but NOT by `transformOffset` or `globalInstanceOffset` — those offsets are applied at ray-trace time). This is called after `finalizeGeometry`.

**INVARIANT:** a face's triangles are either all emissive or none, and their area lights are
consecutive in the order of the triangles. `PerFaceData::localAreaLightIdx` stores the first
triangle's light and `getAreaLightIdxFromHit` adds the hit triangle's index within the face.
`addAreaLights` asserts this, so a chunk that pushed a quad's triangles out of order would fail
there rather than sample the wrong light.

## BLAS Build Throttling

`makeQueuedBlases` builds a bounded number of BLASes per frame (an eighth of its queue, clamped to [8, 64]). Any frame that builds a visible BLAS triggers a dirty TLAS rebuild that same frame, so new instances enter the TLAS together with the area light structure rebuild (see the invariant in [scene.md](scene.md)).
