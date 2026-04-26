_Last edited: 2026-04-26_

# Instance

`Instance` in `src/scene/scene.h/cpp` represents one ray-traceable object in the scene — a BLAS with associated metadata.

## Lifecycle

1. `Scene::requestNewInstance()` allocates on main thread (or reuses a freed instance's vectors).
2. Worker thread fills `host_verts`, `host_idxs`, `host_perTriDatas` directly (public vectors).
3. Worker calls `finalizeGeometry()` to mark data as ready.
4. Main thread calls `Scene::markInstanceReadyForBlasBuild()`.
5. `Scene::makeQueuedBlases()` uploads geometry to GPU, builds BLAS, writes `InstanceData`.
6. On destruction, `Instance::reset()` frees all buffer sections and returns the ID to the pool.

## Vector Reuse (`instancesToReuse`)

When an instance is freed, its `unique_ptr` is moved to `instancesToReuse` rather than destroyed. The next `requestNewInstance` steals the (now-empty) vectors via `stealVectors` — this reuses heap allocations from the previous instance's vectors, avoiding repeated large allocations for terrain chunks that create/destroy instances frequently.

## Visibility

`setVisible(false)` excludes the instance from the TLAS without destroying its BLAS. This is how chunks outside render distance but within `createBlasDistance` are hidden — their geometry stays on the GPU but isn't traversed.

## Area Lights

`addAreaLights()` builds `AreaLight` structs from specified triangle indices. These store world-space vertex positions (transformed by the instance's float transform, but NOT by `transformOffset` or `globalInstanceOffset` — those offsets are applied at ray-trace time). This is called after `finalizeGeometry`.

## BLAS Build Throttling

`makeQueuedBlases` builds at most `maxBlasBuildsPerFrame` (8) BLASes per frame. TLAS rebuild is deferred until either the queue drains or `maxNumWaitingBlases` (64) visible instances accumulate — this batches the expensive TLAS rebuild rather than triggering it per-BLAS.
