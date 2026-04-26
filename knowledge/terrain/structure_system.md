_Last edited: 2026-04-26_

# Structure System

`src/terrain/structure/structure.h/cpp` — places multi-block structures (trees, cacti) using grid-based candidate generation with minimum-distance rejection.

## Placement Algorithm

Each `StructureGen` divides the world into a grid with `gridCellSideLength`-sized cells. Each cell deterministically produces one candidate position (RNG seeded by cell world-position + structure type). Candidates are rejected if any of the 8 surrounding cells' candidates are within `minRadius`. This is cheaper than Poisson disk sampling but achieves similar spacing guarantees.

Additional rejection: must be in this chunk's bounds, on valid ground (heightfield > 0), matching biome, not underwater (unless flagged).

## Cross-Chunk Filling

Structures can extend beyond their origin chunk (e.g. palm trees with ±12 block bounds). The `structureNeighbors` system solves this:
- `checkStructureNeighbors()` builds a list of all chunks in a `structureMaxChunkRadius` (1) neighborhood.
- Each chunk fills structure blocks from **all** neighbors' structure lists, using `StructureBounds` for early AABB rejection.
- The RNG for each structure is seeded by its world position, so it produces identical geometry regardless of which chunk is filling it.

## `tryPlaceStructureBlock`

Only writes if the target is AIR/WATER/WATER_TOP. This means structures can't carve into each other or the terrain — first-placed wins. Since all chunks fill from the same deterministic structure list, ordering doesn't matter.

## Helper Functions

`structure_helpers.h` provides `fillLine` (3D Bresenham), `buildSpline` (de Casteljau Bezier), and `placeLeafCap` (radial disc with tapering radius). These handle chunk-bounds clipping internally so structure generators don't need to.
