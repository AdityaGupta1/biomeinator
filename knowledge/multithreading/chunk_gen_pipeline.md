_Last edited: 2026-10-07_

# Chunk Generation Pipeline

The terrain system uses five task types, all dispatched through the same thread pool but with implicit ordering enforced by the chunk state machine.

## Task Types (in pipeline order)

1. **generateTerrain** — 3D noise sampling, block fill, structure candidate creation. Heaviest task. Throttled per frame (see [terrain → terrain_manager.md](../terrain/terrain_manager.md#task-throttling)).
2. **checkStructureNeighbors** — marks this chunk ready in its 3×3 structure neighbors' masks and pulls theirs. Lightweight. Enqueued immediately when a chunk reaches `HAS_TERRAIN` within fill distance.
3. **fillStructuresAndDecorators** — reads neighbors' structure lists, writes structure blocks + decorators. Medium weight. Only runs once all structure neighbors are ready.
4. **generateSegments** — classifies 4×8×4 segments as AIR/SOLID_SURROUNDED/MIXED. Requires neighbor block data. Uses scratch memory from the allocator.
5. **createInstances** — per-face mesh generation into Instance vertex and per-face buffers. Requires pre-allocated Instances from the main thread.

## Ordering Enforcement

No explicit barriers or dependency graphs exist. Ordering is emergent from the state machine: each task advances the chunk's state upon completion, and the terrain manager only enqueues the next task type when the required state is reached. Worker threads never directly enqueue follow-up tasks for the same chunk — they hand it to the main thread's revisit list (`Terrain::addChunkToRevisit`) and it is scheduled next frame.

## Why Main-Thread Gating Matters

`createInstances` needs `Instance*` pointers allocated from the scene (which is not thread-safe). The terrain manager allocates these on the main thread before enqueuing the geometry task. This is why there's a separate `createInstancesTasks` deque — those tasks need main-thread setup before entering the pool, and are held there while the scene's host geometry cap is reached (see [scene → instance.md](../scene/instance.md)).

## Region Pins

Each task pins the regions around its chunk while it is queued or running, so the main thread
never evicts a region a task can touch. See [terrain → region_system.md](../terrain/region_system.md#pins-and-why-removal-is-safe).

## Completion Callbacks

`createInstances` does not advance its chunk's state. It reports through `Terrain::addChunkWithNewGeometry()`, and the main thread advances the chunk to `HAS_GEOMETRY`, then either queues its BLAS build or, if the chunk left range while meshing, destroys its instances. Only the main thread detects leaving range, so this way it sees each chunk either still generating (and marks it) or done (and destroys it). With the worker advancing, the mark could land just after the worker checked it, leaving a chunk with geometry out of range that nothing destroyed.
