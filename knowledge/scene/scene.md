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
dispatches (`WaterDisplacer`) and BLAS refits, but only for the subset within the circular
animation radius that `Terrain::update` sets from the `waterAnimationDistance` setting *and*
inside the padded view frustum: at render distance 40 a world has ~3,500 water chunks, and
refitting all of them was most of a 7 ms main thread and 3.6 ms of GPU per frame. The subset
is cached in `animatedDeformables` and rebuilt when the radius, its chunk-quantized center,
the frustum normals or the set change, so a locked camera never rebuilds and a turning one
rebuilds every frame (a few thousand cheap tests).

Static and animated water meet without a seam because the wave *amplitude* fades to zero
towards both limits (`waveFade` in `water_waves.hlsli`, driven by `WaveFadeParams`): radially
over the eight chunks before the animation distance, and angularly over a band past the frustum
edge, measured as the sine of the angle so it is a plane-distance test. A chunk is therefore
flat by the time it leaves the set and starts flat when it enters. Water within
`WATER_FOV_EXEMPT_FAR` of the camera ignores the frustum so a turn never reveals a frozen
surface at the player's feet. The same params travel in `RenderParams` and in the displacement
constants, and every consumer of the wave height or gradient applies them: the displacement
pass, the motion vector delta in the G-buffer, and the shading normal's analytic gradient. The
noise perturbation of the shading normal is deliberately not faded, since it never moves
geometry. The frustum normals come from `Camera::getFrustumSideNormals_WS` at the current
field of view, so the zoom key narrows the animated region with the view; the membership
test pads wider than the shader's outer band and adds the chunk's bounding sphere (from the
instance bounds `finalizeGeometry` records), so anything the shaders could still animate is
always in the set. A camera jump or a fast turn can still take a chunk out mid-wave, so
instances leaving the set get one displacement dispatch with `waveScale` 0 plus a refit, and
`freeInstance` erases from the cached subset so that comparison never sees a freed pointer.
`sampleMeshWaveOffsetY` needs no fade term because it is only sampled at the camera, where
the fade is 1.

The cost of animated water is mostly not the refit itself: refit BLASes trace slower than
built ones, and path tracing over the visible water grows with the animated radius, roughly
0.3 ms of path tracing plus 0.15 ms of refit per 4 chunks of radius at render distance 40
(3.1 ms path tracing at 16 chunks, 3.4 at 20, 3.8 at 24, measured with a four-chunk band).
The frustum limit removes about a third of the refits at a given radius. `waterAnimationDistance`
(default 24, fading from 16) is the knob. Displacement rewrites verts **in place** in
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
