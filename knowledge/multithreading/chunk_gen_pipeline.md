_Last edited: 2026-04-26_

# Chunk Generation Pipeline

The terrain system uses five task types, all dispatched through the same thread pool but with implicit ordering enforced by the chunk state machine.

## Task Types (in pipeline order)

1. **generateTerrain** — 3D noise sampling, block fill, structure candidate creation. Heaviest task. Throttled to 12/frame.
2. **checkStructureNeighbors** — builds the 3×3 neighbor list and increments atomic counters. Lightweight. Enqueued immediately when a chunk reaches `HAS_TERRAIN` within fill distance.
3. **fillStructuresAndDecorators** — reads neighbors' structure lists, writes structure blocks + decorators. Medium weight. Only runs once all structure neighbors are ready.
4. **generateSegments** — classifies 4×8×4 segments as AIR/SOLID_SURROUNDED/MIXED. Requires neighbor block data. Uses scratch memory from the allocator.
5. **createInstances** — per-face mesh generation into Instance vertex/index buffers. Requires pre-allocated Instances from the main thread.

## Ordering Enforcement

No explicit barriers or dependency graphs exist. Ordering is emergent from the state machine: each task advances the chunk's state upon completion, and the terrain manager only enqueues the next task type when the required state is reached. Worker threads never directly enqueue follow-up tasks for the same chunk — they signal `setDirty()` and the main thread picks it up next frame.

## Why Main-Thread Gating Matters

`createInstances` needs `Instance*` pointers allocated from the scene (which is not thread-safe). The terrain manager allocates these on the main thread before enqueuing the geometry task. This is why there's a separate `chunksToGenerateGeometry` deque — those chunks need main-thread setup before becoming tasks.

## Completion Callbacks

Two task types report back via mutex-guarded vectors rather than just setting state:
- `createInstances` → `Terrain::addChunkToCreateBlas()` (main thread needs to call `markInstanceReadyForBlasBuild`).
- Chunks that finish geometry while marked for destruction → `Terrain::addChunkToDestroy()`.
