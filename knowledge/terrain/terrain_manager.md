_Last edited: 2026-05-03_

# Terrain Manager

`src/terrain/terrain.h/cpp` — the `Terrain` namespace drives the chunk lifecycle each frame based on camera position.

## Distance Zones

The update loop defines concentric Chebyshev-distance zones that stack outward from render distance:

```
renderDistance (user setting)
  + 1                            = createBlasDistance       (geometry built)
  + 1 + structureMaxChunkRadius  = fillStructuresDistance   (structures + segments advance)
  + structureMaxChunkRadius      = generateTerrainDistance  (noise generated)
```

The extra padding exists so chunks have time to progress through the state machine before reaching visible range. A chunk that enters `createBlasDistance` without having geometry yet would pop in visibly — the padding prevents that.

The `+ structureMaxChunkRadius` term inside `fillStructuresDistance` (not the obvious `+ 1`) is the non-obvious one. For a chunk at `D = createBlasDistance` to reach `HAS_GEOMETRY`, its 4 cardinal neighbors (at `D±1`) must each reach `HAS_ALL_BLOCKS`, which requires every chunk in each cardinal's 5×5 structure footprint (chunks at `D±1±structureMaxChunkRadius`) to have run `checkStructureNeighbors`. That task is enqueued only when a chunk advances `HAS_TERRAIN → AWAITING_STRUCTURE_NEIGHBORS`, which is gated at `fillStructuresDistance`. Dropping the term leaves the `D = renderDistance` ring stuck at `HAS_ALL_BLOCKS` under a locked camera. Moving cameras hide the bug because outer rings keep promoting.

`generateTerrainDistance = fillStructuresDistance + structureMaxChunkRadius` similarly guarantees that the 5×5 footprint of every `fillStructuresDistance` chunk has materialised `Chunk*` objects (`checkStructureNeighbors` walks neighbor pointers and asserts non-null).

## Destruction Uses Union of Old + New Bounds

The scan iterates the union of the previous and current distance bounds. Chunks that were within `createBlasDistance` of the **previous** camera position but are now outside the **current** `createBlasDistance` get destroyed. This ensures a chunk visible last frame won't be missed even if the camera moved far in one frame.

## Task Throttling

Terrain generation tasks (`maxNumGenerateTerrainTasksPerFrame = 12`) are throttled separately from other tasks (`maxTasksPerFrame = 48`) because they're the heaviest (3D noise sampling). Other task types share the 48-task budget via a single deque, processed FIFO.

## Dirty Flag

Worker threads call `Terrain::setDirty()` when they complete a stage that may unblock other chunks. Without this, the update loop would only re-scan when the camera moves, leaving unblocked chunks stuck until the player walks.
