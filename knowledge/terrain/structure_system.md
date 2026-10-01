_Last edited: 2026-09-22_

# Structure System

Surface structures separate geometry from placement rules. The default uses a padded XZ
grid; an opt-in exposed-surface mode finds ledges and fits/thins plants in three dimensions.

## Default ground grid

Each `StructureGen` overlays a world-space grid of `gridCellSideLength` cells. Each cell deterministically produces exactly one candidate (RNG seeded by cell corner + structure type), jittered within an inner region inset by `gridCellPadding` on the cell's **high edge only** (low edge flush to the corner). Because there is one candidate per cell and the inset reserves `gridCellPadding` blocks before the next cell, candidates in adjacent cells are always at least `gridCellPadding + 1` apart — spacing is guaranteed without ever measuring distance. This replaced an earlier scheme that scanned the 8 neighbour cells and rejected on a `minRadius`.

The padding is one-sided rather than centred purely for spacing resolution: one-sided gives every integer min-distance, whereas symmetric padding only reaches odd values. The visual difference (jitter biased toward one corner vs centred) is negligible at structure scale.

**Staggered rows:** odd grid rows are shifted by `gridCellSideLength / 2` in x, breaking the square-lattice column alignment so structures don't form visible rows (approximates hexagonal packing of cell centres). The shift is derived from the global grid-row index, so it stays deterministic across chunk boundaries.

Because the candidate is a pure function of the (global) cell corner, any chunk overlapping a cell computes the identical candidate, and exactly one chunk — the one whose bounds contain the candidate XZ — emplaces it. The high-edge inset also means no neighbouring cell's candidate can ever land inside this chunk, so only cells overlapping the chunk are iterated (no padded neighbour ring).

Additional rejection: must be in this chunk's bounds, on valid ground (heightfield > 0), matching biome, not underwater (unless flagged).

**Weighted variants:** a `StructureGen` holds a weighted list of structure types sharing one grid; the type is rolled per accepted candidate (seeded by candidate position). This is how mixed forests keep different tree types spaced from each other — all variants inherit the grid's spacing guarantee, so cross-type spacing needs no distance checks. The candidate grid is salted by a fold-hash of the variant list (`gridSalt`), which is what keeps multiple gens in the same biome on distinct grids.

**Gotcha:** spacing is only guaranteed within a `StructureGen` — candidates of different gens are never checked against each other, so their structures can overlap.

**Gotcha:** `StructureType` values are serialized by value in world exports (8-bit packed field), so new types must be appended to the enum, never inserted.

## Exposed-surface placement

Setting a gen's `surfacePlacement` replaces its grid with an actual surface scan. Every
eligible upward-facing support is considered, including shelves below the highest surface
in a column. The ground-block whitelist belongs to the rule, and cave-air anchors are excluded
before cave metadata is released. A grid can't do this: even one scanning several Y surfaces
per grid point misses narrow ledges between its sparse XZ points.

Each variant supplies a supported footprint, a clear trunk envelope and horizontal/vertical
spacing. Configure these for the geometry when opting in. Neighbors may be one block lower
than the anchor for support; the anchor itself must be a full solid cube. Clearance checks
actual terrain air, not a heightfield. The clearance radius controls how much space is reserved
around the trunk; zero requires a clear trunk while allowing foliage to meet a backing cliff.
Weighted selection considers only variants that fit, so short
plants fill low shelves without type-specific fallback logic. Tianzi enables the mode for
pines and shrubs; the placement code knows nothing about that biome or those tree types.
Terrain still controls soil and exposed rock independently.

Spacing uses world-XYZ hash priorities and ellipsoidal exclusion volumes. A fitting candidate
with higher priority (smaller hash) suppresses a nearby one even if it is itself suppressed
elsewhere. This is
deliberately a local thinning rule, not a recursive greedy packing algorithm: the latter could
depend on arbitrarily distant sites or chunk generation order. The larger spacing of the two
variants wins. Different elevation shelves can coexist, and shrubs need less room than pines.
As with the grid, separate gens do not compete with each other.

Each destination buckets a gen's candidates on an XZ grid over the neighborhood whose cells
are at least that gen's largest spacing, so competitors are found in the adjacent 3x3 cells
instead of an all-pairs scan (one candidate per exposed surface makes the all-pairs cost
quadratic). Buckets are ranges of one index array (counting sort). Candidates farther than
geometry reach plus spacing from the destination are dropped first: they can neither reach it
nor suppress a candidate that does. Accepted structures are still filled in neighbor order,
which every destination shares, so overlapping trees resolve identically across chunk borders.

Terrain publishes immutable surface candidates alongside its immutable air/full-cube masks.
Filling waits for the existing 3x3 neighborhood, and each destination independently resolves
the candidates whose geometry can reach it. Never consult mutable neighbor blocks, release
the candidates after filling, or read another chunk's in-progress accepted list. A bound on
**geometry reach + competition reach + fit-probe reach** must fit the ready terrain halo;
an assertion enforces it when a rule is used. Opting a much wider structure into this mode
may require expanding that dependency halo, not just increasing its spacing values.

The owner additionally records accepted structures for export. `getStructures()` combines
those with ordinary grid structures by value; exports retain their existing format and do
not serialize transient candidates or pointers into biome configuration. Imported final
blocks and accepted structures retain the existing import behavior.

## Cross-Chunk Filling

Structures can extend beyond their origin chunk (e.g. palm trees with ±12 block bounds). The `structureNeighbors` system solves this:
- `checkStructureNeighbors()` builds a list of all chunks in a `structureMaxChunkRadius` (1) neighborhood.
- Each chunk fills structure blocks from **all** neighbors' structure lists, using `StructureBounds` for early AABB rejection.
- The RNG for each structure is seeded by its world position, so it produces identical geometry regardless of which chunk is filling it.

## `tryPlaceStructureBlock`

Only writes if the target is AIR/WATER/WATER_TOP. Structures cannot carve into each other or
terrain: first-placed wins. Keep overlapping structures in consistent world order in every
destination chunk; worker scheduling is independent of that per-chunk fill order.

## Helper Functions

`structure_helpers.h` provides `fillLine` (3D Bresenham), `buildSpline` (de Casteljau Bezier), `placeLeafCap` (radial disc with tapering radius), and `placeLeafBlob` (y-squashed sphere). These handle chunk-bounds clipping internally so structure generators don't need to.

Tianzi's taller pine variant has short trunks and separate, shallow foliage whorls with
visible trunk between them. Those gaps are intentional: the user's later pine/spruce
references supersede the earlier request to cover every log in the canopy. A continuous
per-Y taper made solid cones, and widening that taper's tip produced boxy crowns. Instead,
vary the spacing and outline of the boughs, taper their widths gently, and use a single small
asymmetric cap over the trunk end. Sparse outer drooping leaves add thickness without filling
the gaps around the trunk. Draw fringe randomness before chunk clipping. The blobby shrub
retains its separate shape. Keep the tall variant's required headroom synchronized with its
highest leaf layer when changing its height range.

Pine trunks may replace pine foliage. Trees rooted at different elevations on cliff steps
can have overlapping crowns even with anchor spacing; first-write-wins for leaves otherwise
left gaps in a later trunk. Logs win over pine leaves regardless of fill order, while terrain
and other solid blocks remain protected. The trunk fill draws no RNG and clips per voxel.

**Local-ground scanning:** fill functions get no heightfield, but `blocks` already contains
generated terrain, so a fill function can scan a column downward to seat sub-features on local
ground (cypress knees do this). Two constraints: the scan must draw no RNG — all params are
drawn unconditionally before it — so the stream invariant below holds, and accepted ground
blocks are whitelisted to natural ground blocks because the scan also sees previously filled
structure blocks, whose presence can vary with fill order.

**RNG stream invariant:** every chunk overlapping a structure fills it with an identically-seeded RNG, so a fill function must consume the same RNG stream in every chunk. Draw all randomness unconditionally (or gated only on RNG-derived/chunk-independent conditions) before any chunk-bounds check — never inside an `isInChunk` branch that affects later draws. The helpers are safe because they clip internally after their own draws.
