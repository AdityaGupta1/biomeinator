_Last edited: 2026-09-20_

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

Terrain generation tasks (`maxNumGenerateTerrainTasksPerFrame = 32`) are throttled separately from other tasks (`maxTasksPerFrame = 256`) because they're the heaviest (3D noise sampling). Other task types share the budget via a single deque, processed FIFO.

The caps used to be 12 and 48, which held a backlog of ~470 tasks with the workers 18% busy on an initial render-distance-40 load. With the larger caps the workers sit at ~90% and generation is bound by terrain generation CPU time itself. The pool is FIFO, so with a deep queue the order tasks are pushed matters: `createInstances` tasks go in ahead of new `generateTerrain` tasks, otherwise chunks one step from visible starve behind hundreds of heavy terrain tasks and the scene can go dozens of frames without a chunk landing.

`maxBlasBuildsPerFrame` (default 32) is the matching cap on the render side; at 8 it was the binding limit on generation time. Beyond 32 the workers are the limit. The per-frame cap is proportional to the queue, an eighth of it clamped to [8, setting], so a load drains at the setting while a row of chunks becoming eligible while moving lands over several frames; 32 of them landing in one frame was the most frequent frame spike, and a hard threshold between the two rates would make a draining load visibly change speed.

## Water Animation Distance

`Terrain::update` hands the scene the circle within which water instances animate together with the wave fade radii: the fade ends at `waterAnimationDistance` (chunks) and starts eight chunks before it, and the set's radius extends a further two and a half chunks past the end so chunks leave it flat. The frustum side of the limit is set by the renderer, not here. See [scene → scene.md](../scene/scene.md#deformable-instances) for why the fade exists and what animated water costs.

## Dirty Flag

Worker threads call `Terrain::setDirty()` when they complete a stage that may unblock other chunks. Without this, the update loop would only re-scan when the camera moves, leaving unblocked chunks stuck until the player walks.
