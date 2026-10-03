_Last edited: 2026-10-02_

# Region System

`Region` is defined in `src/terrain/chunk.h` alongside `Chunk`. Groups 32×32 chunks into a spatial unit.

## Why Regions Exist

Chunks need O(1) neighbor access for segment generation and structure filling. Without regions, every neighbor lookup would be a hash-map query on the global chunk map. Instead, a chunk holds a `Region*` pointer and can traverse to adjacent regions via the region's 4-direction neighbor array.

## Lifetime

Regions live in `unordered_map<ivec2, unique_ptr<Region>>` in the `Terrain` namespace. When
chunks leave render distance only their instances are freed; the `Chunk` keeps its blocks so
nearby back-and-forth movement never regenerates terrain. Whole regions are evicted (with
`--evictRegions`, on by default) once they are far enough away, which is what bounds CPU memory
while exploring: a generated chunk holds about 300 KB that nothing else ever frees. An evicted
region is simply dropped, and the normal pipeline regenerates its chunks if the camera returns.
Generation is deterministic, so they come back identical, but a chunk imported from an older
world comes back as this build generates it. Evicted regions are not in later exports.

### Distances

Both are measured from the camera chunk to the region's nearest chunk. A region is *staged*
beyond `evictRegionDistance` and unstaged within `keepRegionDistance`; between the two it keeps
its state, so a camera moving back and forth across one boundary cannot evict and regenerate a
region repeatedly. `evictRegionDistance` is past the ring of neighbor regions the scan creates
(`generateTerrainDistance + regionSideLength`): evicting those would only see them recreated,
empty, on the next chunk crossing. Staging is only re-evaluated when the camera changes chunk.

### Pins, and why removal is safe

Every task pins the regions within a per-task radius of its chunk from when it is queued until it
returns (`Task::pinnedRegions`). A staged region is removed at the end of `Terrain::update` once
nothing pins it. The radius covers more than the chunks a task reads or writes: removal steps
survivors' readiness back (see [chunk_state_machine.md](chunk_state_machine.md#removal-and-readiness)),
so a task must also pin every region its *writes* to other chunks' readiness depend on. That
is why the neighbor check pins twice the structure radius and the structure fill pins at least
two. Otherwise a worker could advance a survivor in the same instant the main thread steps it back.

- Pins live in the task, not the chunk: a chunk's previous task is often still in its tail
  (after it advanced the state) when the main thread queues the next one.
- `makePinnedTask` creates missing regions in its box. A region created after the pin would be
  unpinned and could be removed under the running task.
- Which regions to remove is decided *before* draining the BLAS and destroy lists, and the
  revisit list is filtered before deletion. A task pushes to these lists before it unpins, so
  zero pins seen first guarantees every pointer a removed region's tasks pushed is drained.
- Queued `generateTerrain` tasks for staged regions are cancelled when popped, so a deep
  backlog left behind a fast camera does not keep far regions pinned.
- `ThreadPool::shutdown` discards queued tasks without unpinning; that is only correct because
  every caller also destroys all regions.

### Freeing without stutter

Freeing large buffers while running stalls frames on this machine, whichever thread does it:
destroying a region's ~1,000 chunks took about 100 ms on the main thread, and freeing their
256 KB block arrays and 32 KB masks from a low-priority thread still stalled `present` for 30 ms,
even paced. So a low-priority thread destroys removed regions (the main thread only unlinks them),
and their chunks' blocks and both masks go to a pool in `chunk.cpp` that new chunks generate into.

- The three buffers are pooled as one set, so a chunk always takes and returns all of them; pools
  per buffer drift apart, since a chunk has two masks but one block array.
- The pool is uncapped. A cap frees its overflow, which brought the stalls back after long flights
  as evictions finished. It stays bounded because chunks take from it before allocating: pooled and
  resident sets together never exceed the most chunks ever resident, plus any still queued for the
  deleter thread.
- Reimport stops the deleter thread before emptying the pool, since an imported world does not
  generate into it. Shutdown destroys the regions explicitly rather than during static destruction,
  where `~Chunk` would depend on the pool in another file still existing.

The heap was not returning that memory to the OS anyway, so process memory is not affected.

### Validating

`--validateEviction` records a hash of each generated chunk's final blocks and block states when
its region is evicted, and compares it when the chunk next finishes its structure pass. Only
mismatches are logged, as errors, one per chunk; imported chunks are skipped, since they are
regenerated from the seed. A headless random walk at a small render distance
(`--renderDistance=8 --perfMoveSpeed=200 --perfMoveTurnFrames=500 --perfFrames=8000`, seed 100)
evicts around a hundred regions and revisits about 8,000 chunks.

To confirm the validator can still fail after changing it, plant a fault that alters regenerated
blocks without crashing, such as skipping the structure pass after the first few thousand fills.
Faults in the readiness bookkeeping tend to crash rather than mismatch, because chunks then fill
against neighbors that have no terrain.

## Neighbor Wiring

`setNeighbor()` is bidirectional — it sets both directions in one call. The terrain manager wires region neighbors when the scan reaches a region, creating missing neighbors, so chunk-level `setNeighbors()` can traverse into adjacent regions. Removing a region clears its neighbors' links and their chunks' cardinal links into it; the scan rewires both when the region returns, since a cleared link drops `getNumNeighborsSet()` below 4.

## Index Order

`chunkPosToIdx`: x fastest, then z. This matches the iteration order in the terrain manager's scan loop.
