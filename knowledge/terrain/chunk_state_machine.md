_Last edited: 2026-04-26_

# Chunk State Machine

Each chunk has an `atomic<ChunkState>` that progresses strictly forward from `NEEDS_TERRAIN` to `HAS_GEOMETRY`. States are ordered as enum values so `>=` comparisons work for "at least this far along" checks.

## Three Transition Drivers

**Main-thread**: the terrain manager's scan checks state and enqueues the next task (e.g. `NEEDS_TERRAIN` → `GENERATING_TERRAIN`).

**Worker-thread**: a completed task advances to the "done" state (e.g. `GENERATING_TERRAIN` → `HAS_TERRAIN`) and calls `setDirty()` to trigger a re-scan.

**Dependency-driven** (the non-obvious ones):
- `AWAITING_STRUCTURE_NEIGHBORS` → `NEEDS_FILL_STRUCTURES`: the last structure neighbor's `checkStructureNeighbors()` increments an atomic counter to the threshold. This can fire on any worker thread.
- `HAS_ALL_BLOCKS` → `NEEDS_SEGMENTS`: triggered when the 4th cardinal neighbor finishes `fillStructuresAndDecorators()` (atomic `numNeighborsWithBlocks` reaches 4). Also self-checks in case this chunk is the last of its own neighbors to finish.

## `advanceState()` Semantics

Compare-exchange loop that only succeeds if current state < target. Returns whether **this thread** performed the advance. This is critical: multiple threads may try to advance the same chunk (e.g. two neighbors both see numNeighborsWithBlocks == 4 due to race). Only the winner's `true` return should enqueue work.

## Destruction Is Partial

When a chunk leaves range, only the GPU mesh is destroyed (instances freed, state reset to `NEEDS_GEOMETRY`). Block data, biomes, segments all survive — re-entering range only requires rebuilding geometry, not re-running noise.

If a chunk is mid-`GENERATING_GEOMETRY` when it leaves range, `isMarkedForDestruction` is set so the completing task destroys it immediately rather than submitting a BLAS that would be instantly torn down.
