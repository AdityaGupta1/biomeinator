_Last edited: 2026-09-09_

# Cave Structure System

`src/terrain/structure/cave_structure.h/cpp` — underground structures that attach
to the floor or ceiling of cave air pockets. Reuses the surface
[structure_system.md](structure_system.md) grid+padding placement and the same
two-pass split, but the *deciding-where* pass diverges (see below). Per-biome
gens live on `CaveBiomeData.caveStructureGens`
([cave_biome_system.md](cave_biome_system.md)), keyed by `CaveBiome` of the
floor/ceiling solid.

## Terrain air mask: the one safe cross-chunk read during fill

A structure is filled once by every chunk it overlaps, and a fill may only touch its
own chunk's blocks. That is fine for shapes that are a pure function of the seed,
but a growth process that must avoid rock (`LAMP_CLUSTER`) needs to know which
blocks are air across the *whole* footprint, including the parts in neighbouring
chunks. Reading a neighbour's `blocks` during fill is a data race — neighbours run
their own structure pass concurrently and mutate `blocks` in place.

`Chunk::terrainAirMask` is the answer: one bit per block, captured from `blocks`
right before `HAS_TERRAIN` and never written again. The structure pass is gated on
every neighbour being `>= HAS_TERRAIN` (acquire), and the mask is written before the
release on that state advance, so `isTerrainAir_WS` can read any neighbour's mask
race-free. It reports *terrain* air (pre-structure), which is exactly what makes it
consistent: every chunk sees the same answer regardless of how far each has got in
its own fill. Cost is `chunkSizeY / 8` bytes per column, 16 KB per chunk.

Imported chunks build the mask from their loaded blocks (which already include
structures), so a cluster whose footprint reaches into an imported chunk sees its
structure blocks as solid there. Deterministic per import; accepted.

The lookup is limited to the 3×3 structure neighbourhood. A footprint radius
≤ `chunkSizeXZ / 2` guarantees every chunk a structure touches has all of the
footprint's chunks within its own neighbourhood.

## Fill order is type-major, then neighbour, then emission order

`runStructuresAndDecoratorPass` fills cave structures one `CaveStructureType` at a
time in enum order, iterating the neighbour grid inside each type. Enum order is
therefore a priority: every lamp in the 3×3 neighbourhood is placed before any vine
cluster anywhere in it, so a vine's ceiling search sees the finished lamp and skips
the column instead of hanging from it or being punched through by a lamp filled
later from another chunk. The neighbour-then-emission order inside a type is still
world-position-fixed, so overlaps within a type stay deterministic. This differs
from surface structures, which fill per neighbour in one pass.

## Same two passes, no new pass or threading

Cave structures slot into the existing chunk pipeline with **zero** new state-
machine states or tasks: positions are decided in Pass 1
(`fillTerrainBlocksAndCreateStructures`, fills `caveStructures`); blocks are
written in Pass 2 (`fillCaveStructureBlocks`), called from
`runStructuresAndDecoratorPass` inside the same `structureNeighbors` loop that
fills surface structures. Both passes share `structureMaxChunkRadius = 1`, so the
existing neighbor gather and gating already cover cave structures — a footprint may cross a chunk border by up to `chunkSizeXZ / 2` blocks (vines reach radius 6) and still lie within every overlapping chunk's neighborhood.

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
at the floor/ceiling **once per layer**. The separate per-voxel cave-air biome
array belongs to decorator surface discovery and is not used to choose structures.

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
- **Floor flora finds its own footing per column.** `MOSS_PINK_CLUSTER` searches a
  few blocks up and down from the anchor for air over a cave-flora ground block
  (`CaveBiomes::isCaveFloraGroundBlock`), so clusters follow the floor and never
  land on bare stone or clay. Per-column seeding as for vines.
- **Vines never anchor to emissive blocks.** `isCaveVinesCeilingBlock` rejects
  `markAsEmitter` cubes so a strand can't hang from a lamp's underside; combined with the
  type-major fill order this keeps lamps and vines from interleaving.
- **A placement requires the candidate column to itself have a qualifying layer.**
  The grid picks one candidate XZ per (cell, type, layerIdx); the structure
  appears only if that specific column owns a layer at that index meeting
  biome/height. Neighboring columns with better pockets do not substitute — this
  is intended one-per-cell behavior.
- **Ceiling gens only run on `closed` layers** (a pocket open to the sky has no
  ceiling solid to hang from). LUSH's `LAMP_CLUSTER` and `CAVE_VINES` are the
  ceiling gens; each is naturally absent wherever its biome
  doesn't generate. Within a biome's gen list, order is priority for a *shared
  candidate column* only — different gens roll different candidate columns per cell,
  so a cell can host one of each.
- **`CRYSTAL_CLUSTER` and `CRYSTAL_CLUSTER_HANGING` are one fill written in terms of a grow
  direction**, since a hanging cluster is the exact mirror of a standing one. They are separate
  types only because the fill has no other way to know which side of the pocket it was anchored to,
  and separate types also give them independent placement grids, so a pocket can host both.
- **A cluster's emitter and its glass are deliberately separate.** The CRYSTAL_CORE mound is bare
  rather than sheathed, so its light reaches the cave directly and only picks up colour and
  scattering from whichever WHITE_CRYSTAL knees happen to stand in its path. An earlier version
  sealed the core inside glass, which left it lit by BSDF-sampled refraction alone (see
  [shaders → materials.md](../shaders/materials.md)) — dark and noisy, and the sealed prisms read as
  too geometric.
- **Knees seat themselves by scanning their own column**, the way cypress knees do
  ([structure_system.md](structure_system.md)), which is why every knee's parameters are drawn
  *before* its surface scan and its chunk-bounds check: a chunk fills only its own columns, so any
  draw skipped inside those branches would desynchronise the RNG stream between chunks. It accepts
  only the biome's own rock, so a knee can't climb the mound this fill just wrote or perch on an
  earlier structure's blocks.
- **A cluster's reach is what `CAVE_STRUCTURE_BOUNDS` must cover**, and the knees set it, not the
  mound: they scatter several times further out than the mound's radius. The bound is a constant
  (`crystalClusterMaxReachXZ`) that has to be revisited whenever the knee distance changes. Reach
  must stay under `chunkSizeXZ` for the 3x3 gather to cover every chunk the cluster touches; the
  tighter `chunkSizeXZ / 2` limit above applies only to fills that read the neighbourhood air mask,
  which this one does not.
- **`availableHeight` users:** `STONE_COLUMN` fills floor→ceiling for `end - start`
  blocks; `CAVE_VINES` caps strand length at `availableHeight - 1` so a strand never
  touches the floor. The fixed-height gens ignore it; their high `minLayerHeight`
  guarantees clearance. `tryPlaceStructureBlock` is AIR-only, so a 3×3 pillar
  auto-clips per column to whatever air actually exists.
- **`chance` is rolled per (cell, type, layerIdx) after the candidate match**, from a
  stream independent of the candidate-position RNG. A failed roll falls through to
  the next gen in the list rather than leaving the cell empty, so gen-list order is
  still priority order.
- **Multi-column fills must seed per column from world position.** A structure is
  filled once by every chunk it overlaps, and each fill visits only that chunk's
  columns, so a single RNG advanced across the footprint would desynchronise between
  chunks. `CAVE_VINES` seeds each strand from (column XZ, anchor y) the way cypress
  spanish moss does; the strand's chance, length and berry variants all come from
  that per-column stream. Overlapping structures are deterministic because fill order
  is world-position row-major over neighbours then emission order within a chunk —
  first writer wins into AIR and every chunk agrees on who was first.

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
