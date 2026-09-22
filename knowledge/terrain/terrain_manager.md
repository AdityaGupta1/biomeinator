_Last edited: 2026-09-21_

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

Terrain generation tasks (`maxNumGenerateTerrainTasksPerFrame = 96`) are throttled separately from other tasks (`maxTasksPerFrame = 512`) because they're the heaviest (3D noise sampling). Other task types share the budget via a single deque, processed FIFO.

The caps used to be 12 and 48, which held a backlog of ~470 tasks with the workers 18% busy on an initial render-distance-40 load. After the September 2026 generator speedup the limits moved again: at 32/256 with a BLAS cap of 32 the load took 5.1 s with the workers 23% busy; 96/512 with a BLAS cap of 64 takes 3.2 s at 56% busy, with the per-frame period during the load about 2 ms worse at p95 (denser frames) and nothing binding but the pipeline itself. The caps only matter while a backlog exists, so they do not change streaming while moving. The pool is FIFO, so with a deep queue the order tasks are pushed matters: `createInstances` tasks go in ahead of new `generateTerrain` tasks, otherwise chunks one step from visible starve behind hundreds of heavy terrain tasks and the scene can go dozens of frames without a chunk landing.

The BLAS build cap in `Scene::makeQueuedBlases` is the matching limit on the render side; at 8 it was the binding limit on generation time, and after the generator speedup 32 was again. The per-frame cap is proportional to the queue, an eighth of it clamped to [8, 64], so a load drains at the setting while a row of chunks becoming eligible while moving lands over several frames; 32 of them landing in one frame was the most frequent frame spike, and a hard threshold between the two rates would make a draining load visibly change speed.

## Water Animation Distance

`Terrain::update` hands the scene the circle within which water instances animate together with the wave fade radii: the fade ends at `waterAnimationChunks` (24) and starts eight chunks before it, and the set's radius extends a further two and a half chunks past the end so chunks leave it flat. The frustum side of the limit is set by the renderer, not here. See [scene → scene.md](../scene/scene.md#deformable-instances) for why the fade exists and what animated water costs.

## Revisit List and Dirty Flag

The full scan over every chunk within generate distance costs about a millisecond per
thousand chunks squared of range (1.2 ms at render distance 50), so it only runs when the
camera changes chunk. Worker threads that advance a chunk's state, their own or a
neighbour's whose dependency counter they completed, push it through
`Terrain::addChunkToRevisit`; the next update runs the same per-chunk scheduling
(`scheduleChunkWork`) on just those chunks, with the "last" camera chunk equal to the current
one so the enter/leave logic is a no-op. The list is taken before a full scan and discarded
by it, since the scan covers everything in range and a chunk out of range has nothing to
schedule. `Terrain::setDirty()` remains for the cases that need the full scan without a camera
move: world import and reset.

`renderDistance` is read every update but is not meant to change at runtime (for the time
being): the zones above are only re-evaluated for every chunk on a camera chunk change, so a
live edit would take effect on the next crossing rather than immediately, and nothing resizes
the per-distance buffers or the light structures for it.
