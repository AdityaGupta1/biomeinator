_Last edited: 2026-05-24_

# Structure System

`src/terrain/structure/structure.h/cpp` — places multi-block structures (trees, cacti) using grid-based candidate generation. Spacing is enforced by construction (padding), not by any pairwise distance check.

## Placement Algorithm

Each `StructureGen` overlays a world-space grid of `gridCellSideLength` cells. Each cell deterministically produces exactly one candidate (RNG seeded by cell corner + structure type), jittered within an inner region inset by `gridCellPadding` on the cell's **high edge only** (low edge flush to the corner). Because there is one candidate per cell and the inset reserves `gridCellPadding` blocks before the next cell, candidates in adjacent cells are always at least `gridCellPadding + 1` apart — spacing is guaranteed without ever measuring distance. This replaced an earlier scheme that scanned the 8 neighbour cells and rejected on a `minRadius`.

The padding is one-sided rather than centred purely for spacing resolution: one-sided gives every integer min-distance, whereas symmetric padding only reaches odd values. The visual difference (jitter biased toward one corner vs centred) is negligible at structure scale.

**Staggered rows:** odd grid rows are shifted by `gridCellSideLength / 2` in x, breaking the square-lattice column alignment so structures don't form visible rows (approximates hexagonal packing of cell centres). The shift is derived from the global grid-row index, so it stays deterministic across chunk boundaries.

Because the candidate is a pure function of the (global) cell corner, any chunk overlapping a cell computes the identical candidate, and exactly one chunk — the one whose bounds contain the candidate XZ — emplaces it. The high-edge inset also means no neighbouring cell's candidate can ever land inside this chunk, so only cells overlapping the chunk are iterated (no padded neighbour ring).

Additional rejection: must be in this chunk's bounds, on valid ground (heightfield > 0), matching biome, not underwater (unless flagged).

**Gotcha:** spacing is per-type only — candidates of different structure types are never checked against each other, so two different structures can overlap.

## Cross-Chunk Filling

Structures can extend beyond their origin chunk (e.g. palm trees with ±12 block bounds). The `structureNeighbors` system solves this:
- `checkStructureNeighbors()` builds a list of all chunks in a `structureMaxChunkRadius` (1) neighborhood.
- Each chunk fills structure blocks from **all** neighbors' structure lists, using `StructureBounds` for early AABB rejection.
- The RNG for each structure is seeded by its world position, so it produces identical geometry regardless of which chunk is filling it.

## `tryPlaceStructureBlock`

Only writes if the target is AIR/WATER/WATER_TOP. This means structures can't carve into each other or the terrain — first-placed wins. Since all chunks fill from the same deterministic structure list, ordering doesn't matter.

## Helper Functions

`structure_helpers.h` provides `fillLine` (3D Bresenham), `buildSpline` (de Casteljau Bezier), and `placeLeafCap` (radial disc with tapering radius). These handle chunk-bounds clipping internally so structure generators don't need to.
