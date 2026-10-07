_Last edited: 2026-10-06_

# Instance

`Instance` in `src/scene/scene.h/cpp` represents one ray-traceable object in the scene — a BLAS with associated metadata.

## Lifecycle

1. `Scene::requestNewInstance()` allocates on main thread, taking pooled CPU geometry vectors if any.
2. Worker thread fills `hostGeometry.verts`, `.idxs`, `.perFaceDatas` directly (public vectors),
   and sets `trisPerFaceLog2` if a `PerFaceData` entry covers more than one triangle. Terrain
   also fills `hostGeometry.packedTerrainVerts` and decodes `.verts` back from it, so the BLAS
   build (from the staging upload) and the area lights use the same rounded geometry the shaders
   read; only the packed form goes resident. See
   [shaders → common_structs.md](../shaders/common_structs.md).
3. Worker calls `finalizeGeometry()` to mark data as ready.
4. Main thread calls `Scene::markInstanceReadyForBlasBuild()`.
5. `Scene::makeQueuedBlases()` uploads geometry to GPU, builds BLAS, writes `InstanceData`, then
   returns its `hostGeometry` to the scene's pool.
6. On destruction, `Instance::reset()` frees all buffer sections and returns the ID to the pool.

## CPU Geometry Is Only Kept Until Upload (`hostGeometryPools`)

Nothing reads an instance's `hostGeometry` after `makeQueuedBlases` copies it into the staging
buffers: refits, compaction, area lights and the TLAS all use the GPU copies. So an instance
returns its vectors, emptied but keeping their capacity, right after upload, and new instances
take them. Keeping them for each live instance cost about 2.8 MB of CPU memory per visible chunk,
about half of everything committed at render distance 30.

The pools never free anything while running, because freeing these buffers stalls frames: trimming
four sets took 20–40 ms on the main thread, and up to 26 ms even with generation idle (see also
[terrain → region_system.md](../terrain/region_system.md#freeing-without-stutter)). Instead,
Terrain only meshes a chunk while fewer than `maxTerrainInstancesHoldingHostGeometry` (512) terrain
instances hold geometry, so the large pool can never hold more sets than that. Streaming stays far
below the cap; only loads reach it. At render distance 30 the pool tops out around 880 MB and the
load takes 3.5 s, against 3.0 s uncapped; a cap of 256 halves the pool but takes 5 s. Sets keep
the capacity of the largest mesh they have held, so the pool can creep above that over a long
session, but never past 512 sets.

Pooled sets keep their capacity, so there are two pools (`HostGeometrySize`). Water meshes are
tiny, and drawing them from the same pool tied up terrain-sized sets and half the cap; water uses
the small pool and is not capped.

Freed instances are destroyed in `freeInstance`, so it removes the instance from every list that
holds a raw pointer, `pendingTlasEntryAdds` included, before erasing it.

## Visibility

`setVisible(false)` excludes the instance from the TLAS without destroying its BLAS. This is how chunks outside render distance but within `createBlasDistance` are hidden — their geometry stays on the GPU but isn't traversed.

## Per-Face Data

`PerFaceData` is stored per mesh face, not per triangle: glTF instances have one entry per
triangle (`trisPerFaceLog2 == 0`), terrain and water one per quad (`== 1`), which halves the
buffer for terrain since both triangles of a quad always carried identical data. The shader
maps `PrimitiveIndex() >> trisPerFaceLog2` to the entry. A quad's first triangle is always
even; a custom model's lone triangle fills a whole face with a degenerate second triangle.
`trisPerFaceLog2 == 1` also means the indices are implicit (`Instance::hasQuadFaces`), so
`hostGeometry.idxs` stays empty for terrain and water.

## Area Lights

`addAreaLights()` builds `AreaLight` structs from specified triangle indices. These store world-space vertex positions (transformed by the instance's float transform, but NOT by `transformOffset` or `globalInstanceOffset` — those offsets are applied at ray-trace time). This is called after `finalizeGeometry`.

**INVARIANT:** a face's triangles are either all emissive or none, and their area lights are
consecutive in the order of the triangles. `PerFaceData::localAreaLightIdx` stores the first
triangle's light and `getAreaLightIdxFromHit` adds the hit triangle's index within the face.
`addAreaLights` asserts this, so a chunk that pushed a quad's triangles out of order would fail
there rather than sample the wrong light.

## BLAS Build Throttling

`makeQueuedBlases` builds a bounded number of BLASes per frame (an eighth of its queue, clamped to [8, 64]). Any frame that builds a visible BLAS triggers a dirty TLAS rebuild that same frame, so new instances enter the TLAS together with the area light structure rebuild (see the invariant in [scene.md](scene.md)).
