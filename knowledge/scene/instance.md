_Last edited: 2026-09-20_

# Instance

`Instance` in `src/scene/scene.h/cpp` represents one ray-traceable object in the scene — a BLAS with associated metadata.

## Lifecycle

1. `Scene::requestNewInstance()` allocates on main thread (or reuses a freed instance's vectors).
2. Worker thread fills `host_verts`, `host_idxs`, `host_perFaceDatas` directly (public vectors),
   and sets `trisPerFaceLog2` if a `PerFaceData` entry covers more than one triangle. Terrain
   also fills `host_packedTerrainVerts`, in which case `host_verts` only feeds the BLAS build
   (from its staging upload) and the area lights, and the packed form is what goes resident; see
   [shaders → common_structs.md](../shaders/common_structs.md).
3. Worker calls `finalizeGeometry()` to mark data as ready.
4. Main thread calls `Scene::markInstanceReadyForBlasBuild()`.
5. `Scene::makeQueuedBlases()` uploads geometry to GPU, builds BLAS, writes `InstanceData`.
6. On destruction, `Instance::reset()` frees all buffer sections and returns the ID to the pool.

## Vector Reuse (`instancesToReuse`)

When an instance is freed, its `unique_ptr` is moved to `instancesToReuse` rather than destroyed. The next `requestNewInstance` steals the (now-empty) vectors via `stealVectors` — this reuses heap allocations from the previous instance's vectors, avoiding repeated large allocations for terrain chunks that create/destroy instances frequently.

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
