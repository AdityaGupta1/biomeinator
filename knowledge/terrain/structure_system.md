_Last edited: 2026-09-21_

# Structure System

`src/terrain/structure/structure.h/cpp` — places multi-block structures (trees, cacti) using grid-based candidate generation. Spacing is enforced by construction (padding), not by any pairwise distance check.

## Placement Algorithm

Each `StructureGen` overlays a world-space grid of `gridCellSideLength` cells. Each cell deterministically produces exactly one candidate (RNG seeded by cell corner + structure type), jittered within an inner region inset by `gridCellPadding` on the cell's **high edge only** (low edge flush to the corner). Because there is one candidate per cell and the inset reserves `gridCellPadding` blocks before the next cell, candidates in adjacent cells are always at least `gridCellPadding + 1` apart — spacing is guaranteed without ever measuring distance. This replaced an earlier scheme that scanned the 8 neighbour cells and rejected on a `minRadius`.

The padding is one-sided rather than centred purely for spacing resolution: one-sided gives every integer min-distance, whereas symmetric padding only reaches odd values. The visual difference (jitter biased toward one corner vs centred) is negligible at structure scale.

**Staggered rows:** odd grid rows are shifted by `gridCellSideLength / 2` in x, breaking the square-lattice column alignment so structures don't form visible rows (approximates hexagonal packing of cell centres). The shift is derived from the global grid-row index, so it stays deterministic across chunk boundaries.

Because the candidate is a pure function of the (global) cell corner, any chunk overlapping a cell computes the identical candidate, and exactly one chunk — the one whose bounds contain the candidate XZ — emplaces it. The high-edge inset also means no neighbouring cell's candidate can ever land inside this chunk, so only cells overlapping the chunk are iterated (no padded neighbour ring).

Additional rejection: must be in this chunk's bounds, on valid ground (heightfield > 0), matching biome, not underwater (unless flagged).

Tianzi's tiered formations also support trees on side shoulders. Its candidates scan exposed
grass surfaces in the owning column, including lower shelves above the formation ground.
Vertical clearance selects between pines and shrubs; vertical spacing prevents planting
several overlapping trees under one crown. This is a surface-only exception, not general cave
tree placement. The same global XZ grid still owns the column, with world Y added to the variant
seed so selecting one shelf cannot shift another shelf's random stream.

**Weighted variants:** a `StructureGen` holds a weighted list of structure types sharing one grid; the type is rolled per accepted candidate (seeded by candidate position). This is how mixed forests keep different tree types spaced from each other — all variants inherit the grid's spacing guarantee, so cross-type spacing needs no distance checks. The candidate grid is salted by a fold-hash of the variant list (`gridSalt`), which is what keeps multiple gens in the same biome on distinct grids.

**Gotcha:** spacing is only guaranteed within a `StructureGen` — candidates of different gens are never checked against each other, so their structures can overlap.

**Gotcha:** `StructureType` values are serialized by value in world exports (8-bit packed field), so new types must be appended to the enum, never inserted.

## Cross-Chunk Filling

Structures can extend beyond their origin chunk (e.g. palm trees with ±12 block bounds). The `structureNeighbors` system solves this:
- `checkStructureNeighbors()` builds a list of all chunks in a `structureMaxChunkRadius` (1) neighborhood.
- Each chunk fills structure blocks from **all** neighbors' structure lists, using `StructureBounds` for early AABB rejection.
- The RNG for each structure is seeded by its world position, so it produces identical geometry regardless of which chunk is filling it.

## `tryPlaceStructureBlock`

Only writes if the target is AIR/WATER/WATER_TOP. This means structures can't carve into each other or the terrain — first-placed wins. Since all chunks fill from the same deterministic structure list, ordering doesn't matter.

## Helper Functions

`structure_helpers.h` provides `fillLine` (3D Bresenham), `buildSpline` (de Casteljau Bezier), `placeLeafCap` (radial disc with tapering radius), and `placeLeafBlob` (y-squashed sphere). These handle chunk-bounds clipping internally so structure generators don't need to.

Pine canopy layers keep a minimum radius of one wherever they overlap the trunk. Otherwise
the taper can select only the center voxel, which leaf placement cannot replace because it is
a log, leaving bare bark under the tip. The radius adds exactly four cardinal leaves in that
case. Determine overlap from the generated trunk span, not a read of the local center block:
neighboring chunks can own those leaves while the trunk lies outside their bounds. Leaf-only
layers above the trunk retain their narrow tip.

**Local-ground scanning:** fill functions get no heightfield, but `blocks` already contains
generated terrain, so a fill function can scan a column downward to seat sub-features on local
ground (cypress knees do this). Two constraints: the scan must draw no RNG — all params are
drawn unconditionally before it — so the stream invariant below holds, and accepted ground
blocks are whitelisted to natural ground blocks because the scan also sees previously filled
structure blocks, whose presence can vary with fill order.

**RNG stream invariant:** every chunk overlapping a structure fills it with an identically-seeded RNG, so a fill function must consume the same RNG stream in every chunk. Draw all randomness unconditionally (or gated only on RNG-derived/chunk-independent conditions) before any chunk-bounds check — never inside an `isInChunk` branch that affects later draws. The helpers are safe because they clip internally after their own draws.
