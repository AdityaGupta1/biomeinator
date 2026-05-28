_Last edited: 2026-05-27_

# Cave Structure System

`src/terrain/structure/cave_structure.h/cpp` — underground structures that attach
to the floor or ceiling of cave air pockets. Reuses the surface
[structure_system.md](structure_system.md) grid+padding placement and the same
two-pass split, but the *deciding-where* pass diverges (see below). Per-biome
gens live on `CaveBiomeData.caveStructureGens`
([cave_biome_system.md](cave_biome_system.md)), keyed by `CaveBiome` of the
floor/ceiling solid.

## Same two passes, no new pass or threading

Cave structures slot into the existing chunk pipeline with **zero** new state-
machine states or tasks: positions are decided in Pass 1
(`fillTerrainBlocksAndCreateStructures`, fills `caveStructures`); blocks are
written in Pass 2 (`fillCaveStructureBlocks`), called from
`runStructuresAndDecoratorPass` inside the same `structureNeighbors` loop that
fills surface structures. Both passes share `structureMaxChunkRadius = 1`, so the
existing neighbor gather and gating already cover cave structures — the 3×3 (radius
1) footprint can cross a chunk border and is reassembled from neighbors' lists
exactly like surface.

## Column-centric placement (the key divergence)

Surface placement is **cell-centric**: iterate grid cells, compute one candidate
per cell, emit from the cell. That requires the whole heightfield finished first
(a cell's candidate can map to any column), which is why surface uses a separate
loop after block-fill.

Cave placement is **column-centric**: the decision for a column needs only *that
column's own* air pockets, which are fully known the instant its y-scan ends. So
placement is interleaved directly into the block-fill loop, right after each
column's scan, and layers live in a single `std::vector<CaveLayer>` scratch that
is cleared and refilled per column (never persisted, one column's handful of
pockets alive at a time). No separate pass, no per-column layer storage.

This forces the **predicate form** of the grid math: "is *this* column's XZ the
candidate of its grid cell, for this gen at this layer index?" The staggered-row +
high-edge-inset math is factored into `gridCellCornerForPosXZ_WS` +
`gridCellCandidateXZ_WS` (in `chunk_generator.cpp`), shared with surface. Surface
keeps bit-identical seeding (`worldSeed ^ hash(87152059)`, type as arg4), so its
goldens are unaffected by the refactor.

## Layer capture is free

`CaveLayer` (in `cave_biome.h`) is captured during the existing block-fill
y-scan with **no new noise samples**: cave-air is the already-computed `isCave`
flag, and the floor/ceiling cave biome reuses the per-voxel biome already
classified for the base-block choice. Floor event (solid→cave-air) opens a layer;
ceiling event (cave-air→solid) closes it `closed=true`; cave-air→non-cave-air
closes it `closed=false` (opened to sky — ceiling gens skipped). Biome is sampled
at the floor/ceiling **once per layer**, never per voxel, preserving the
"no per-voxel cave biome storage" invariant in cave_biome_system.md.

## Why the seed folds in `layerIdx` (required, not optional)

The placement seed is `worldSeed ^ hash(magic) ^ hash(type) ^ hash(layerIdx·k)`.
The `layerIdx` term is **load-bearing**: a feature-pos column has the same XZ for
all of its stacked pockets, so without it every pocket in that column would hash
identically and stamp the structure in *all* of them. Folding the count-from-
bottom layer index in gives each pocket an independent grid.

## Gotchas

- **Layer-index seam.** Because the seed uses `layerIdx` (a count-from-bottom),
  the "same physical pocket" can straddle two different grids at a pinch-point
  where a lower pocket pinches out and relabels the indices of those above. This
  locally breaks the spacing/coverage guarantee (possible clumping or dropout at
  pinch-points only). Accepted: pinch-points are thin, structures are sparse
  decoration, and pocket interiors are constant-index and coherent. A quantized-
  floor-y seed would just trade this for height-bucket seams.
- **Redundant predicate math, accepted.** Every column in a cell recomputes that
  cell's candidate and only one matches (grid 12 → ~144 columns recompute to
  return true once). Tiny vs the 80k+ block-fill iterations; columns with no
  captured layers skip the gen loop entirely, so non-cave columns cost nothing.
- **A placement requires the candidate column to itself have a qualifying layer.**
  The grid picks one candidate XZ per (cell, type, layerIdx); the structure
  appears only if that specific column owns a layer at that index meeting
  biome/height. Neighboring columns with better pockets do not substitute — this
  is intended one-per-cell behavior.
- **Ceiling gens only run on `closed` layers** (a pocket open to the sky has no
  ceiling solid to hang from). Currently only BRIMSTONE has a ceiling gen
  (`OBSIDIAN_STALACTITE`); it is naturally absent wherever hot-dry caves don't
  generate.
- **`STONE_COLUMN` is the only gen using `availableHeight`** (fills floor→ceiling
  for `end - start` blocks). The fixed-height gens ignore it; their high
  `minLayerHeight` guarantees clearance. `tryPlaceStructureBlock` is AIR-only, so
  a 3×3 pillar auto-clips per column to whatever air actually exists.

## Not serialized (parity gap with surface)

`caveStructures` is **not** written to the world export, unlike surface
`structures`. Imported chunks (`wasImported`) skip both passes and load blocks
with cave structures already baked in, so this only matters for the overhang of a
cave structure whose origin sits in an imported chunk but spills into a freshly-
generated neighbor — that overhang is lost. Surface avoids this by serializing its
list; cave structures accept the gap (decorative, and the case is rare). Voxel-
mode golden tests load imported worlds, so they do **not** exercise cave-structure
generation — regenerating those goldens means re-exporting the world dump (Ctrl+U)
with current code.
