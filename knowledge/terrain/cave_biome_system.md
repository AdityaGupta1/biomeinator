_Last edited: 2026-09-07_

# Cave Biome System

`src/terrain/cave_biome.h/cpp` — themes underground stone using two 3D noise
fields (temperature and humidity), classified into a 2D biome space. Distinct
from the surface [biome_system.md](biome_system.md): the fields are 3D, so cave
biomes vary with **y** and the same column can pass through several with depth.

## Why a separate enum from `Biome`

Surface `Biome` selection is partitioned by `inland` and is inherently 2D
(one biome per column). Cave biomes are a different concept — 3D, no inland
axis, and their only job (for now) is choosing which block replaces `STONE`.
Overloading `Biome` would drag in the irrelevant inland partitioning and
per-column assumptions, so `CaveBiome` is its own enum + data table with the
same nearest-neighbor-by-`distance2` shape, kept deliberately extensible (add
an enum entry + one init block).

## STONE at the origin

`STONE` sits at the origin of noise space; the special biomes sit at the
corners. Because selection is nearest-neighbor, `STONE` only wins near the
center, so the special biomes naturally appear only where the noise is strong —
no explicit rarity threshold needed. Adding more special biomes just means more
corners; STONE keeps the middle.

## Surface bias

The effective classification noise is the 3D field plus the column's 2D surface
temperature/humidity scaled by `caveBiomeSurfaceBias`. This loosely anchors a
cave biome to whatever is above it (a humid surface tends toward LUSH below) while
the 3D term lets it drift with depth. The 2D arrays already exist in scratch for
surface biome selection, so the bias is effectively free.

## Downsampling + seam alignment (gotcha)

The two 3D fields are generated on a **coarse grid**, one sample per
`caveBiomeNoiseDownsample` (4) blocks per axis, and trilinearly interpolated in
the fill loop. Biome regions are far larger than a block, so this costs ~1/64 of
a full-resolution field with no visible difference, and the smoothing also kills
single-voxel biome speckle.

Critical invariant: the coarse grid origin is the **chunk origin**, which is a
multiple of `chunkSizeXZ` (16) and therefore of the downsample factor. Adjacent
chunks thus sample identical world positions on their shared border, so biomes
stay seamless across chunks. The buffer carries `+1` cell on each XZ axis (the
far-edge interpolation margin overlapping the next chunk's first cell) and `+2`
in y. If the downsample factor ever stops dividing `chunkSizeXZ`, the coarse
origin must be explicitly snapped or borders will mismatch.

## No per-voxel storage

The biome's block effects (base block and skin) are classified on the fly inside
the fill loop and baked straight into the block choice — nothing is stored per
voxel. The base block covers **all** solid stone with `y < caveNoiseMaxY` (not
just cave walls), so exposed faces anywhere in the band read as the biome. Cave
structures read the biome once per captured layer at fill time
([cave_structure_system.md](cave_structure_system.md)); a per-voxel store is still
not needed.

## Secondary rock

`secondaryBaseBlock` / `secondarySkinFringeBlock` let a biome alternate between two
rock types (LUSH: stone and marble, each with its own overgrown fringe) on a
separate low-frequency coarse field (`fnCaveRock`, ~160-block features, thresholded
at `caveSecondaryRockThreshold`). It is sampled on the same downsampled grid as the
other cave fields, so the rock boundary is smooth and seam-free across chunks; the
skin and clay are rock-agnostic and lie on top of whichever rock is chosen.

## Surface skin via carve noise (no distance pass)

`skinBlock` / `skinPatchBlock` theme only the shell of solid rock around cave
surfaces — floors, walls and ceilings alike — leaving `baseBlock` deeper in (LUSH
is stone with a moss skin and clay patches). The "distance to the cave surface" is
**not** measured: the fill loop already has the voxel's carve noise and the carve
threshold, and `noise - threshold` grows with distance from the carved surface in
every direction, so a voxel is skin when that difference is below a thickness
expressed in noise units (`caveSkinThicknessMin/Max`). This costs no extra pass,
no neighbor reads, and has no chunk-border seam.

`skinFringeBlock` is a second band immediately outside the skin (`thickness +
caveSkinFringeWidth`). On the exposed surface it therefore appears exactly where
the thickness field is slightly negative — the ring between a skin patch and bare
rock — which is how LUSH gets overgrown stone between moss and plain stone
without any neighbour lookup. The fringe is only committed to voxels with air
directly above them (the block has a moss-capped side texture, so it must read as
a floor): the scan is bottom-up, so a fringe candidate is written as `baseBlock`
and promoted one iteration later when the voxel above turns out to be AIR. The thickness range must dip further negative than
the fringe width or bare rock never shows.

Two consequences to know about:
- The thickness field is coarse 3D noise (same downsampled grid as the biome
  fields) and its range dips negative, which is what produces bare-stone patches
  on the surface rather than a uniform coat.
- Skin also forms around *near-misses* — rock where the noise came close to
  carving but didn't. Those pockets are buried and invisible unless something
  else exposes them (a ravine, a structure); mega-minecraft's vertical-only
  variant had the analogous artifact. Accepted.

Noise-unit thickness maps to different block depths in the worley and simplex
cave bands because their gradients differ (worley is steeper), so the skin reads
slightly thinner in deep rounded caves than in shallow spaghetti caves.
