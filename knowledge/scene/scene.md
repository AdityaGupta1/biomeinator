_Last edited: 2026-07-24_

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

**INVARIANT:** the TLAS instance set must only change on dirty rebuilds, which also rebuild
the area light structures — non-dirty per-frame rebuilds just refresh AABBs. This holds
because `makeQueuedBlases` dirties the TLAS on any frame that builds a visible BLAS, and
`setVisible` dirties it when toggling an instance with a valid BLAS. If a freshly built
emissive instance entered the TLAS before the sampling structure / light tree knew about it,
the path tracer's light tree lookups for its hits read garbage and can hang the GPU (observed
as intermittent TDR during world import in `cave_lights`).

## Deformable Instances

`Instance::isDeformable` (set by chunk meshing for water) routes an instance into
`deformableInstances` after its first BLAS build. The set drives the per-frame displacement
dispatches (`WaterDisplacer`) and BLAS refits. Displacement rewrites verts **in place** in
the shared verts buffer — no rest-position copy — relying on top verts sitting at k + 7/8
and the wave amplitude staying < 0.125 (see `shaders/common/water_waves.hlsli`). The
whole-buffer UAV transitions around the dispatch also cover terrain verts, so the pass must
not overlap other passes reading verts.

`water_displacer.cpp` also holds a CPU mirror of the shader's `waveHeight()` (constants and
math must be kept in sync) used by `sampleMeshWaveOffsetY()`, which reproduces the
**rendered** surface at a point: corner wave heights interpolated across the two top-face
triangles (diagonal from local (0, 0) to (1, 1), matching `cubeFaceVertPositions` in
`chunk.cpp`) rather than evaluating the wave function directly at that point. The
camera-underwater check in `terrain.cpp` relies on this to agree with the mesh the rays
actually hit.

## Area Light Sampling Structure

Indirection array mapping dense sampling indices `[0, numAreaLights)` to sparse area light
buffer indices. Needed because area lights live in a managed buffer where freed/reordered
instances leave gaps, but uniform sampling needs a contiguous range. Rebuilt every TLAS
rebuild.

## Bounds

Tracks optional world-space bounds for glTF geometry, used by NRC for
`sceneBoundsMin/Max`. Voxel mode uses terrain bounds instead. Bounds intentionally match
the glTF loader's current flat (no parent/child hierarchy) transform behavior — if hierarchy
support is added, bounds expansion should follow.

## Reset

`reset()` clears all arrays and empties the `availableInstanceIds` queue, so
`init()` must be called again afterward to repopulate it. The glTF loader does
exactly this: `scene.reset(); scene.init();`.
