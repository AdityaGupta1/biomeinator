_Last edited: 2026-09-20_

# Scene

`src/scene/scene.h/cpp` owns ray-traceable instances, shared geometry buffers, materials,
textures, TLAS build state, and scene-level metadata needed by rendering systems. Single
point of contact between gameplay/loading code and GPU scene representation.

## Shared Geometry Buffers

Instances don't own their own device buffers. All vertex, index, and per-triangle data lives
in three shared `ReservedManagedBuffer`s. `makeQueuedBlases` copies host vectors into
sections of these and records offsets in `InstanceData`. This avoids per-instance resource
creation overhead in a world with thousands of terrain chunks.

## Per-Frame Instance Descriptors

`D3D12_RAYTRACING_INSTANCE_DESC` arrays are duplicated per frame-in-flight so CPU writes for
frame N don't race with GPU reads from frame N-1. `InstanceData` doesn't need this because
it's copied to a device buffer before use.

## Update Ordering

`Scene::update()` ordering matters — BLAS builds must happen before TLAS rebuild, and TLAS
rebuild must happen before area light sampling structure copy, because TLAS rebuild is what
populates the sampling structure. The return value signals whether accumulation should reset.

Deformable instances (water) are displaced and their BLASes refit between the BLAS builds
and the TLAS rebuild. Once a TLAS exists it is rebuilt **every frame**, because refits change
the BLAS AABBs the TLAS caches (also smooths the frame-pacing spikes of bursty rebuilds). Two
things stay gated on `isTlasDirty` so per-frame deformation doesn't trigger them: the area
light sampling structure rewrite (which would rebuild the light tree and reset accumulation
every frame, hanging test-mode screenshots) and the `didChange` return value.

**INVARIANT:** an instance enters or leaves the TLAS only through `addTlasEntry` /
`removeTlasEntry`, which update the area light sampling structure in the same step. If a
freshly built emissive instance entered the TLAS before the sampling structure / light tree
knew about it, the path tracer's light tree lookups for its hits read garbage and can hang the
GPU (observed as intermittent TDR during world import in `cave_lights`).

## TLAS Instance Entries

`tlasInstanceEntries` is the contiguous list of instances currently in the TLAS, maintained
incrementally rather than rebuilt by walking `instances` every frame: at ten thousand chunks
that walk (plus re-emitting two million area light indices) cost several milliseconds per
frame during streaming, when every frame changes topology. The per-frame rebuild only copies
the entries into the frame's instance desc array with the global offset applied.

- Adds happen where a `ToFreeList` is at hand: `makeQueuedBlases` adds a visible instance as
  its BLAS is built, and `update()` drains `pendingTlasEntryAdds`, which `setVisible(true)`
  fills because it has no list to pass to a possible sampling structure resize.
- Removals (`setVisible(false)`, `freeInstance`) only null the entry's instance pointer and
  set `tlasEntriesNeedCompaction`; `makeTlas` compacts once, so a frame that unloads many
  chunks pays one pass, not one per chunk. Each `Instance` carries its entry index so both
  operations are O(1).
- Transform setters update the entry in place; they also mark `isTlasDirty` so the change
  resets accumulation like any other scene change.

## Deformable Instances

`Instance::isDeformable` (set by chunk meshing for water) routes an instance into
`deformableInstances` after its first BLAS build. The set drives the per-frame displacement
dispatches (`WaterDisplacer`) and BLAS refits, but only for the subset inside the animation
bounds that `Terrain::update` sets from the `waterAnimationDistance` setting: at render
distance 40 a world has ~3,500 water chunks, and refitting all of them cost 10-13 ms of CPU
and 3.6 ms of GPU per frame, which was the whole reason the game was CPU-bound at that
distance. Far water simply keeps its last displacement, which is sub-pixel at that range. The
subset is cached in `animatedDeformables` and rebuilt only when the bounds or the set change,
since even iterating the full set is measurable. Displacement rewrites verts **in place** in
the shared verts buffer — no rest-position copy — relying on top verts sitting at k + 7/8
and the wave amplitude staying < 0.125 (see `shaders/common/water_waves.hlsli`). The
whole-buffer UAV transitions around the dispatch also cover terrain verts, so the pass must
not overlap other passes reading verts.

`water_displacer.cpp` also holds a CPU mirror of the shader's `waveHeight()` (constants and
math must be kept in sync; both take the wrapped `waveTime`, never raw `animTime`, see
`WATER_WAVE_PERIOD_SECONDS`) used by `sampleMeshWaveOffsetY()`, which reproduces the
**rendered** surface at a point: corner wave heights interpolated across the two top-face
triangles (diagonal from local (0, 0) to (1, 1), matching `cubeFaceVertPositions` in
`chunk.cpp`) rather than evaluating the wave function directly at that point. The
camera-underwater check in `terrain.cpp` relies on this to agree with the mesh the rays
actually hit.

## Area Light Sampling Structure

Indirection array mapping dense sampling indices `[0, numAreaLights)` to sparse area light
buffer indices. Needed because area lights live in a managed buffer where freed/reordered
instances leave gaps, but uniform sampling needs a contiguous range. It is maintained with
the TLAS entries: an add appends the instance's range, a compaction rewrites it. The CPU
master copy `areaLightDenseIdxs` exists because the mapped array's staging slots are
per-frame, so an incremental append written into one slot is not present in the others; the
device buffer is the source of truth and each change is staged from the master copy and
marked dirty. It is pre-sized to 2M entries, since a resize during streaming (four new
buffers plus freeing the old ones) showed up as a 30-50 ms frame.

## Reset

`reset()` clears all arrays and empties the `availableInstanceIds` queue, so
`init()` must be called again afterward to repopulate it. The glTF loader does
exactly this: `scene.reset(); scene.init();`.
