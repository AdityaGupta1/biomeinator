# Cave structures

_Last edited: 2026-05-24_

## Goal

Add underground "cave structures" that attach to the floor or ceiling of cave
air pockets, reusing the now-proven grid + padding placement scheme from surface
structures (see [structure_placement_grid_padding.md](structure_placement_grid_padding.md)).

Three structures to start, all 3×3 in XZ:

| type | block | shape | anchor | biome | grid / padding / minLayerHeight |
|---|---|---|---|---|---|
| `CRYSTAL` | `RAINBOW_CRYSTAL` | 5 tall, grows up from floor | first air above floor | `MARBLE_CRYSTALS` | 12 / 4 / 12 |
| `HANGING_LAMP` | `LAMP` | 5 tall, grows down from ceiling | top air below ceiling | `BRIMSTONE` | 12 / 4 / 12 |
| `STONE_COLUMN` | `SCALESTONE` | floor → ceiling, fills full gap | first air above floor | `STONE` | 56 / 12 / 10 |

All three blocks already exist in `block.h`.

---

## Background: how structure placement works today (two passes)

There are two distinct passes, separated in the chunk state machine
(`chunk.h:16`). This separation is the key thing to keep straight.

### Pass 1 — deciding *where* (same task as block fill)

`Chunk::fillTerrainBlocksAndCreateStructures` (`chunk_generator.cpp:286`,
state `NEEDS_TERRAIN → HAS_TERRAIN`) does two things back-to-back in one
function:

1. The block-fill loop (~399–566): a nested loop over columns (z, then x), and
   per column a vertical y-scan that writes `blocks` and computes `heightfield`.
2. The surface structure **creation** loop (568–639): grid-cell-centric. For
   each biome's `StructureGen`, iterate the grid cells overlapping the chunk,
   compute each cell's single jittered candidate XZ (staggered odd rows + high-
   edge padding inset, 586–603), snap to the heightfield, reject on
   biome/underwater/cave-top, and push `{type, pos_WS}` into `this->structures`.
   **No structure blocks are written here** — only positions are decided.

### Pass 2 — writing the *blocks* (separate, later task)

`Chunk::fillStructureBlocks` (`structure.cpp:260`, state
`NEEDS_FILL_STRUCTURES → FILLING_STRUCTURES → HAS_ALL_BLOCKS`). The state
machine gates it: a chunk waits (`AWAITING_STRUCTURE_NEIGHBORS`) until every
chunk within `structureMaxChunkRadius` (1) has terrain and its structure list,
then writes structure blocks into `blocks`, pulling from its **own + all
neighbors'** structure lists (`structureNeighbors`). This is what lets a
structure spanning a chunk border render correctly. `tryPlaceStructureBlock`
writes only into AIR/WATER, so structures never carve terrain and first-placed
wins.

**Takeaway: deciding where = Pass 1; writing blocks = Pass 2.** Cave structures
slot into the same split.

---

## Cave-specific concepts

A surface column has exactly one anchor (its `heightfield` value). A cave column
has N stacked air pockets ("layers"), each with a floor, a ceiling, an available
height, and a (potentially different) cave biome at floor vs ceiling. Two new
sub-problems beyond surface:

1. **Detecting the pockets** and their floor/ceiling biomes.
2. **Choosing which pocket(s) get a structure** without a pairwise distance
   check — handled by the same grid + padding scheme, with the layer index
   folded into the seed.

### Cave layers

A `CaveLayer` records one air pocket for one column:

- `start` — the floor solid block's y (exclusive: not air). First air is
  `start + 1`.
- `end` — the top air block's y (inclusive). Ceiling solid is `end + 1`.
- `bottomBiome` — `CaveBiome` of the floor solid (`y = start`).
- `topBiome` — `CaveBiome` of the ceiling solid (`y = end + 1`).
- `closed` — whether the pocket is capped by a ceiling. A run of cave-air that
  opens upward into non-cave air (toward the surface) has no ceiling solid;
  mark it `closed = false` so ceiling gens are skipped (floor gens may still
  run).

`layerHeight = end - start` = number of air blocks in the pocket, and is the
**available height** carried into the fill function.

### Capturing layers in the block-fill loop

The block-fill loop already tracks `wasSolid`/`isSolid` per voxel
(`chunk_generator.cpp:531`) and classifies the cave biome of every solid voxel
in the cave band for the base-block choice (514). Cave-air is precisely the
existing `isCave` flag. So layer capture is cheap and adds **no new noise
samples**:

- Hoist carry vars alongside `wasSolid`: whether a layer is currently open, its
  `start`, its `bottomBiome`, and the cave biome of the last solid voxel.
- On a **floor event** (solid → cave-air): open a layer, record `start = y - 1`
  and reuse the last solid voxel's biome as `bottomBiome`.
- On a **ceiling event** (cave-air → solid): close the layer with `end = y - 1`
  and the current solid voxel's biome as `topBiome`, mark `closed = true`, push
  it.
- On a pocket opening into non-cave air (cave-air → non-cave non-solid): close
  with `closed = false`, push it.

Layers come out bottom-up for free (y ascending). Biome is re-sampled at the
floor/ceiling **once per layer**, never stored per voxel — preserving the
"no per-voxel cave biome storage" invariant in
[cave_biome_system.md](../knowledge/terrain/cave_biome_system.md).

---

## Placement: column-centric (diverges from surface)

Surface placement is cell-centric: iterate cells, compute one candidate per
cell, emit from the owning cell. Cave placement must be **column-centric**,
because the layer data is per-column and only exists for columns this chunk
generated. The decision for a column needs only **that column's own layers**, so
it can run the instant the column's y-scan finishes.

### Interleave into Pass 1, layers in a scratch buffer

The moment a column's y-scan ends, its layers are fully known. So, **still
inside the block-fill loop, right after the y-scan for that column**, run cave
placement for that column and push decided structures into a persisted
`this->caveStructures` list.

Hold the layers in a single `std::vector<CaveLayer>` that is **cleared at the
top of each column and refilled** — reused across all 256 columns, only one
column's handful of layers alive at a time. This scratch buffer is never
persisted and vanishes when Pass 1 ends.

Rationale (vs. persisting all columns' layers + a separate placement pass):
surface needs a separate pass because a cell's candidate can map to any column,
so the whole heightfield must be finished first. Caves need only the anchor
column's own layers, which are ready immediately — so there is no reason to wait
or to persist. Interleaving costs zero layer storage. The downside (below) is
accepted.

### The `isFeaturePos` predicate

Surface computes a candidate per cell and emits it. Caves need the **predicate
form**: "is *this* column's XZ the place-pos of its grid cell, for this gen at
this layer index?" This reuses the exact surface grid math (staggered odd rows +
high-edge inset, `chunk_generator.cpp:586–603`), factored into a shared helper:
given a world XZ, find its grid cell (accounting for the odd-row x shift),
recompute that cell's candidate, and compare for equality.

Per column, walk its layers bottom-up; for each layer try the floor-biome gens
(anchor `start + 1`) and, if `closed`, the ceiling-biome gens (anchor `end`).
For each gen, in list order, reject unless all of:

- `gen.generatesFromCeiling` matches the floor/ceiling side being tried,
- `layerHeight >= gen.minLayerHeight`,
- not in lava (anchor y above the lava surface, y≈4 — `chunk_generator.cpp:555`),
  unless a "can generate in lava" flag is set,
- the `isFeaturePos` predicate passes.

First passing gen wins per floor / per ceiling per layer (gen-list order =
priority). On success, push `CaveStructure { type, pos_WS, availableHeight =
layerHeight }`.

**Seed:** derive from `worldSeed`, the gen type, and the **layer index** (e.g.
`worldSeed ^ hash(magic) ^ hash(type) ^ hash(layerIdx * k)`), no per-chunk
state — same determinism guarantee as surface. The `layerIdx` term is required:
without it, a feature-pos column would stamp the structure in *every* one of its
stacked layers (same XZ → same seed → all pass).

### Accepted cost and accepted seam

- **Redundant predicate math.** Column-centric runs the predicate per
  (column × layer × gen). Within a grid cell, every column recomputes that
  cell's same candidate and only one matches (grid 12 → ~144 columns recompute
  to return `true` once). This is intentionally accepted: each eval is a tiny
  rng seed + compare, worst case ~1500/chunk, negligible beside the 80k+
  heavier block-fill iterations. Columns with **no** captured layers skip the
  gen loop entirely, so the common non-cave column costs nothing.
- **Layer-index seam.** Because the seed folds in `layerIdx` (a count-from-
  bottom that can relabel between adjacent columns when a lower pocket pinches
  out), the "same physical pocket" can straddle two different grids at a pinch-
  point, locally breaking the spacing/coverage guarantee (possible clumping or
  dropout there). Accepted: pinch-points are thin and the structures are sparse
  decoration; pocket interiors are constant-index and coherent. (A quantized-
  floor-y seed would trade this for height-bucket seams; not worth it now.)

---

## Pass 2: filling cave structure blocks (cross-chunk)

Because the structures are 3×3 (radius 1 in XZ), they **can** cross chunk
borders, so the deferred fill + neighbor gather is required (unlike a width-1
structure). Mirror the surface path:

- A `CaveStructureBounds` table (radius 1 for all three) for AABB early-reject,
  analogous to `StructureBounds` (`structure.h:29`).
- A `fillCaveStructureBlocks` pass alongside `fillStructureBlocks`, reusing the
  same neighbor list and `structureMaxChunkRadius = 1` (16 blocks ≫ 1, fits
  easily). Each chunk fills its own + neighbors' `caveStructures`.
- A fill-func table keyed by `CaveStructureType`, like `Structures`.

Vertical extent never crosses a chunk boundary (chunks are full height), so XZ
bounds suffice. `tryPlaceStructureBlock` (AIR-only) auto-clips a 3×3 structure
where a neighbor column is solid at that y — so `STONE_COLUMN`'s pillar conforms
to whatever air actually exists in each of its nine columns.

### Fill functions

Each loops dx, dz ∈ [-1, 1] with per-block `isInChunkXZ` clamping (like the
surface tree funcs), writing a vertical run:

- `CRYSTAL` — 5 `RAINBOW_CRYSTAL` upward from `start + 1` (anchor). Ignores
  `availableHeight` (fixed 5; `minLayerHeight = 12` guarantees clearance).
- `HANGING_LAMP` — 5 `LAMP` downward from `end` (anchor). Fixed 5; clearance
  guaranteed by `minLayerHeight = 12`.
- `STONE_COLUMN` — `SCALESTONE` from `start + 1` upward for `availableHeight`
  blocks (floor to ceiling). This is the one that uses `availableHeight`.

`LAMP` is emissive, so `HANGING_LAMP` contributes light automatically once its
blocks are placed.

---

## New / changed files

- **`src/terrain/structure/cave_structure.h/.cpp`** (new) — mirrors
  `structure.h/.cpp`: `CaveStructureType` enum, `CaveStructure`,
  `CaveStructureGen`, `CaveStructureBounds`, the fill-func table + `init()`, and
  the three fill functions.
- **`src/terrain/chunk.h`** — add `CaveLayer` (near `CaveBiomeNoise`), a
  `std::vector<CaveStructure> caveStructures` member, and the
  `fillCaveStructureBlocks` declaration.
- **`src/terrain/chunk_generator.cpp`** — layer capture in the block-fill loop;
  the column-centric placement step interleaved after each column's y-scan; the
  shared `isFeaturePos` predicate helper (factored from the surface candidate
  math).
- **`src/terrain/cave_biome.h/.cpp`** — add `std::vector<CaveStructureGen>
  caveStructureGens` to `CaveBiomeData`; populate `MARBLE_CRYSTALS`,
  `BRIMSTONE`, and `STONE` entries in `init()`.

---

## Things to watch

- **Determinism.** Seed `isFeaturePos` purely from world XZ + gen type +
  `layerIdx`, no chunk/gen-order state, so a world column produces identical
  layers and placements on every regen regardless of which chunk fills it.
- **Spacing fits the halo.** Padding 4 with a radius-1 structure → candidate
  centers ≥5 apart → ≥2 air blocks between structure edges, no overlap.
  `STONE_COLUMN` at padding 12 is far sparser. Both well within
  `structureMaxChunkRadius = 1`.
- **Both-in-one-layer collision.** A layer whose floor biome is `MARBLE_CRYSTALS`
  and ceiling biome is `BRIMSTONE` could host both a `CRYSTAL` and a
  `HANGING_LAMP`; with enough height they don't meet, but if they ever did,
  `tryPlaceStructureBlock` is first-wins (harmless). The high `minLayerHeight`
  (12) makes a collision unlikely.
- **Cross-type overlap** is unaddressed, same as surface — spacing is per
  (gen, layerIdx) grid only.

---

## Suggested order

1. `CaveLayer` + capture in the block-fill loop (verify layers look sane, e.g.
   via a temporary debug block).
2. `cave_structure.h/.cpp` skeleton + `CaveBiomeData.caveStructureGens` wiring,
   one structure (`CRYSTAL`) end-to-end: predicate, placement, Pass-2 fill.
3. Add `HANGING_LAMP` (ceiling path) and `STONE_COLUMN` (`availableHeight`
   path).
4. Regenerate affected cave golden images.
5. Add `knowledge/terrain/cave_structure_system.md` (rationale, the column-
   centric divergence, the layer-index seam gotcha).
