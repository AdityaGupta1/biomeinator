_Last edited: 2026-04-26_

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

After `reset()`, `init()` does not need to be called again — arrays are re-created in-place.
