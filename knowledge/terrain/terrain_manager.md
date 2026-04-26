_Last edited: 2026-04-26_

# Terrain Manager

`src/terrain/terrain.h/cpp` — the `Terrain` namespace drives the chunk lifecycle each frame based on camera position.

## Distance Zones

The update loop defines concentric Chebyshev-distance zones that stack outward from render distance:

```
renderDistance (user setting)
  + 1 = createBlasDistance     (geometry built)
  + 1 = fillStructuresDistance (structures + segments advance)
  + structureMaxChunkRadius = generateTerrainDistance (noise generated)
```

The extra padding exists so chunks have time to progress through the state machine before reaching visible range. A chunk that enters `createBlasDistance` without having geometry yet would pop in visibly — the padding prevents that.

## Destruction Uses Union of Old + New Bounds

The scan iterates the union of the previous and current distance bounds. Chunks that were within `createBlasDistance` of the **previous** camera position but are now outside the **current** `createBlasDistance` get destroyed. This ensures a chunk visible last frame won't be missed even if the camera moved far in one frame.

## Task Throttling

Terrain generation tasks (`maxNumGenerateTerrainTasksPerFrame = 12`) are throttled separately from other tasks (`maxTasksPerFrame = 48`) because they're the heaviest (3D noise sampling). Other task types share the 48-task budget via a single deque, processed FIFO.

## Dirty Flag

Worker threads call `Terrain::setDirty()` when they complete a stage that may unblock other chunks. Without this, the update loop would only re-scan when the camera moves, leaving unblocked chunks stuck until the player walks.
