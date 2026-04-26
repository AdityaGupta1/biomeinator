_Last edited: 2026-04-26_

# Region System

`Region` is defined in `src/terrain/chunk.h` alongside `Chunk`. Groups 32×32 chunks into a spatial unit.

## Why Regions Exist

Chunks need O(1) neighbor access for segment generation and structure filling. Without regions, every neighbor lookup would be a hash-map query on the global chunk map. Instead, a chunk holds a `Region*` pointer and can traverse to adjacent regions via the region's 4-direction neighbor array.

## Lifetime

Regions are never destroyed during a session. They live in `unordered_map<ivec2, unique_ptr<Region>>` in the `Terrain` namespace. When chunks leave render distance, only their instances are freed — the `Chunk` object (with its block data) persists inside the region. This avoids regenerating terrain when the player returns to a previously-visited area.

## Neighbor Wiring

`setNeighbor()` is bidirectional — it sets both directions in one call. The terrain manager wires region neighbors immediately upon region creation so that chunk-level `setNeighbors()` can always traverse into adjacent regions.

## Index Order

`chunkPosToIdx`: x fastest, then z. This matches the iteration order in the terrain manager's scan loop.
