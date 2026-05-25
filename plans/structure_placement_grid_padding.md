# Structure placement via grid + padding (Poisson-free spacing)

_Last edited: 2026-05-24_

## Goal

Replace the current surface-structure spacing (pairwise `minRadius` neighbor
check) with a **grid-cell + padding** placement scheme borrowed from the
`mega-minecraft` project. Prove it on surface structures first; if it works,
reuse the exact same mechanism for cave structures (which additionally need
floor/ceiling attachment, a minimum air-gap, and an available-height arg).

The key insight: **spacing and "what is here" are two separate concerns, and
neither needs a pairwise distance check.**

| concern | mechanism | needs neighbor data? |
|---|---|---|
| WHERE a structure can go (spacing) | grid cell + padding, pure function of world XZ + seed | no |
| WHAT is at a spot (height / biome / floor-ceiling) | per-column lookup against already-generated terrain | no |

---

## How it works today (surface structures)

Reference: `src/terrain/chunk_generator.cpp` ~lines 568-702,
`src/terrain/structure/structure.{h,cpp}`.

- Each `BiomeData` carries a list of `StructureGen { type, gridCellSideLength,
  minRadius, flags }`.
- For each biome present in the chunk, for each `StructureGen`:
  - Overlay a world-space 2D grid of `gridCellSideLength`.
  - For every grid cell (padded by ±1 cell into neighbors), seed an rng by the
    cell's world position and pick a **jittered candidate XZ** inside the cell.
  - Snap the candidate to `heightfield[columnIdx]`, reject if cave-top / wrong
    biome / underwater.
  - **Pairwise spacing:** loop the 8 neighboring cells (lines 647-695) and
    reject the candidate if any neighbor candidate of the same type is within
    `minRadius`.
  - Emplace `Structure { type, pos_WS }` into `this->structures`.
- `Chunk::fillStructureBlocks` later runs each structure's fill function. The
  state machine (`NEEDS_FILL_STRUCTURES`, `chunk.h:22`) guarantees every chunk
  within `structureMaxChunkRadius` (currently 1) has terrain first, and a chunk
  fills its own **and its neighbors'** structures so structures spanning chunk
  borders render correctly.

The candidate XZ is already a pure function of grid cell (the rng is seeded by
grid position), which is why neighbor candidates can be recomputed locally
without touching neighbor `Chunk` objects. The pairwise `minRadius` loop is the
only "distance check," and it is what we want to remove.

---

## The mega-minecraft approach

Reference (in `../mega-minecraft`): `src/terrain/chunk.cu`,
`src/terrain/biome.hpp`, `src/terrain/featurePlacement.hpp`.

### Spacing: `isFeaturePos` (chunk.cu:999) — no distance check at all

```cpp
bool isFeaturePos(ivec2 worldBlockPos2d, int gridCellSize, int gridCellPadding, int seed)
{
    const ivec2 gridCorner = ivec2(floor(vec2(worldBlockPos2d) / (float)gridCellSize) * (float)gridCellSize);
    const int internalSide = gridCellSize - (2 * gridCellPadding);
    const vec2 randPos = rand2From3(vec3(gridCorner, seed));
    const ivec2 placePos = gridCorner + ivec2(gridCellPadding) + ivec2(floor(randPos * (float)internalSide));
    return worldBlockPos2d == placePos;
}
```

- One jittered position per grid cell, **inset by `gridCellPadding`** on each
  side.
- Spacing is guaranteed *by construction*: at most one structure per cell, and
  the padding inset means two structures in adjacent cells are at least
  `2 * gridCellPadding` apart. That distance is enforced, never measured.
- Pure function of world XZ + seed, so every chunk computes the same answer for
  any cell. **Zero neighbor data required for spacing.**

This is the same "candidate is a pure function of grid cell" idea biomeinator
already relies on, but it replaces the pairwise `minRadius` loop with a padding
inset — strictly simpler.

### Per-feature-gen config (biome.hpp:187, 214)

`FeatureGen` / `CaveFeatureGen` carry `gridCellSize`, `gridCellPadding`,
`chancePerGridCell` (a probabilistic skip so not every cell is populated), and
for caves `minLayerHeight`, `generatesFromCeiling`, `canGenerateInLava`.

### Cross-chunk gather (chunk.cu:1169)

`otherChunkGatherFeaturePlacements` collects neighbors' placements into
`gatheredFeaturePlacements` / `gatheredCaveFeaturePlacements`. **No dedup, no
pairwise distance** — the global grid makes the lists automatically consistent
across chunks. This is the same shape as biomeinator's existing neighbor-fill.

---

## Plan A — apply to surface structures first

Minimal change to validate the mechanism.

1. Extend `StructureGen` with `gridCellPadding` (and optionally
   `chancePerGridCell` to replace any current probability gating). Keep
   `gridCellSideLength`; drop reliance on `minRadius`.
2. In the candidate loop (`chunk_generator.cpp` ~568-702), replace the jittered
   candidate + 8-neighbor `minRadius` reject (lines 647-695) with an
   `isFeaturePos`-style test:
   - Compute the cell's single padded-jittered position from `(gridCorner,
     seed)`.
   - Keep it only if this column's XZ equals that position.
3. Keep everything else: heightfield snap, biome gate, underwater/cave-top
   rejects, `this->structures.emplace_back`, and the existing neighbor-fill via
   `structureMaxChunkRadius`.
4. **Constraint:** `2 * gridCellPadding` (the min spacing) must fit the gather
   halo. With `structureMaxChunkRadius = 1` (16 blocks) and
   `gridCellSideLength <= chunkSizeXZ`, this holds. Larger cells/padding → bump
   `structureMaxChunkRadius`.
5. Regenerate affected golden images (surface structure layout will shift).

### Things to watch

- The current pairwise check explicitly only compares **same-type** candidates
  within ±1 cell (see the comment at chunk_generator.cpp:648 about not checking
  across types). Padding gives same-cell-grid spacing per type; cross-type
  overlap is still unaddressed, same as today.
- `chancePerGridCell` keeps density tunable now that there is exactly one slot
  per cell.
- Determinism: seed `isFeaturePos` exactly as the current candidate rng is
  seeded (`worldSeed ^ hash(...)` + grid pos + type) so results stay stable
  across chunk boundaries.

---

## Plan B — cave structures (only after A works)

Adds floor/ceiling attachment, min air-gap, and available-height — the parts
that genuinely differ from surface.

### Store cave layers during the fill loop

Mirror mega-minecraft's `CaveLayer`:

```cpp
struct CaveLayer {
    int start;             // exclusive: floor block (solid, not air)
    int end;               // inclusive: top air block
    CaveBiome bottomBiome; // biome sampled at start    (floor)
    CaveBiome topBiome;    // biome sampled at end + 1   (ceiling)
};
```

- The fill loop already tracks `wasSolid`/`isSolid` (chunk_generator.cpp:531)
  and already classifies the cave biome per solid voxel for the base block
  (line 514). On a floor event (solid→air) record `start` + reuse the biome
  classification; on a ceiling event (air→solid) close the layer with `end` +
  ceiling biome. Push to a per-column list on the `Chunk`.
- `layerHeight = end - start` is the **available height**; carry it into the
  cave structure's fill function as an arg (mega-minecraft passes it as
  `CaveFeaturePlacement::layerHeight`, used to scale crystals / cap chain
  length).
- Biome is **re-sampled at the floor/ceiling once per layer**, not stored per
  voxel — preserves the "no per-voxel cave biome storage" invariant in
  `knowledge/terrain/cave_biome_system.md`.

### Placement

- `CaveBiomeData` gains `std::vector<CaveStructureGen>` keyed by floor vs
  ceiling (`generatesFromCeiling`), with `minLayerHeight`.
- Walk each column's cave layers; for each layer try the floor biome's gens
  (anchor `start + 1`) and the ceiling biome's gens (anchor `end`).
- Gate each gen by the same `isFeaturePos` grid+padding test from Plan A, with
  the layer index folded into the seed (mega-minecraft: chunk.cu:1069) so
  stacked layers in one column decorrelate.
- Reject if `layerHeight < minLayerHeight`.
- Store `CaveStructure { type, pos_WS, availableHeight }` and gather across
  chunks exactly like surface `structures`.

### First two cave structures

- `CRYSTAL` — floor gen in `MARBLE_CRYSTALS`.
- `HANGING_LAMP` — ceiling gen in `BRIMSTONE`.

### Storage note

Per-chunk cave-layer lists cost memory in cavey chunks (many pockets × 256
columns). Pre-filter on `layerHeight >= min(minLayerHeight over all gens)` and
drop the lists once `FILL_STRUCTURES` completes. mega-minecraft caps at
`MAX_CAVE_LAYERS_PER_COLUMN = 32` and stores a fixed-size array per column.

---

## Why this beats the earlier ideas

Earlier exploration considered a 3D jittered candidate grid (needs re-sampling
noise + scanning blocks at decision time) and a hash-priority site-bucket
(needs a neighbor-halo gather to thin sites). Both are heavier than necessary.
The grid+padding scheme removes the pairwise/halo distance logic entirely:
spacing is a closed-form function of world position, and the per-column terrain
lookup answers everything else. It is also already proven in mega-minecraft.
