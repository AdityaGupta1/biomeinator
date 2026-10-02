_Last edited: 2026-10-02_

# Chunk State Machine

Each chunk has an `atomic<ChunkState>` that progresses forward from `NEEDS_TERRAIN` to `HAS_GEOMETRY`, except for the main-thread steps back described below. States are ordered as enum values so `>=` comparisons work for "at least this far along" checks.

## Three Transition Drivers

**Main-thread**: the terrain manager's scan checks state and enqueues the next task (e.g. `NEEDS_TERRAIN` → `GENERATING_TERRAIN`).

**Worker-thread**: a completed task advances to the "done" state (e.g. `GENERATING_TERRAIN` → `HAS_TERRAIN`) and calls `Terrain::addChunkToRevisit()` so the main thread schedules that chunk's next stage without a full re-scan.

**Dependency-driven** (the non-obvious ones):
- `AWAITING_STRUCTURE_NEIGHBORS` → `NEEDS_FILL_STRUCTURES`: the last structure neighbor's `checkStructureNeighbors()` completes an atomic bit mask with a bit per neighbor. This can fire on any worker thread.
- `HAS_ALL_BLOCKS` → `NEEDS_SEGMENTS`: triggered when the 4th cardinal neighbor finishes `fillStructuresAndDecorators()` and completes `neighborsWithBlocksMask`. Also self-checks in case this chunk is the last of its own neighbors to finish.

## Removal and readiness

Readiness is a bit mask rather than a counter so neighbors can leave and come back. Each stage
both *pushes* its bit into its neighbors and *pulls* bits from neighbors already far enough
along. Pushing alone would leave a regenerated chunk waiting forever on survivors that announced
themselves before it existed, and a counter would double count a neighbor that announced itself
twice. A concurrent finish is still covered by the push, because each chunk advances its state
before pushing.

When a region is removed, the main thread clears the removed chunks' bits from every survivor
within the structure radius and steps survivors back to the last stage that did not depend on
them: `NEEDS_FILL_STRUCTURES` back to `AWAITING_STRUCTURE_NEIGHBORS`, and anything from
`NEEDS_SEGMENTS` on back to `HAS_ALL_BLOCKS`, discarding segments. That is cheaper than a new
gate before geometry, and those chunks are far from the camera. The in-progress states cannot
occur there: their tasks pin the region (see [region_system.md](region_system.md#pins-and-why-removal-is-safe)).
`structureNeighbors` only exists during the structure pass for the same reason: a survivor
keeping it would hold pointers into removed chunks.

## `advanceState()` Semantics

Compare-exchange loop that only succeeds if current state < target. Returns whether **this thread** performed the advance. This is critical: multiple threads may try to advance the same chunk (e.g. two neighbors both see numNeighborsWithBlocks == 4 due to race). Only the winner's `true` return should enqueue work.

## Destruction Is Partial

When a chunk leaves range, only the GPU mesh is destroyed (instances freed, state reset to `NEEDS_GEOMETRY`). Block data, biomes, segments all survive — re-entering range only requires rebuilding geometry, not re-running noise. The chunk itself only goes when its whole region is evicted.

If a chunk is mid-`GENERATING_GEOMETRY` when it leaves range, `isMarkedForDestruction` is set so the completing task destroys it immediately rather than submitting a BLAS that would be instantly torn down.
