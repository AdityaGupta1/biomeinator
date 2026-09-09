_Last edited: 2026-09-09_

# Cave Biome System

`src/terrain/cave_biome.h/cpp` — themes underground stone using two 3D noise
fields (temperature and humidity), classified into a 2D biome space. Distinct
from the surface [biome_system.md](biome_system.md): the fields are 3D, so cave
biomes vary with **y** and the same column can pass through several with depth.

## Why a separate enum from `Biome`

Surface `Biome` selection is partitioned by `inland` and is inherently 2D
(one biome per column). Cave biomes are a different concept — 3D, no inland
axis, and they own their own theming (base and secondary rock, surface skin,
structure gens, surface decorator) rather than the surface data.
Overloading `Biome` would drag in the irrelevant inland partitioning and
per-column assumptions, so `CaveBiome` is its own enum + data table with the
same nearest-neighbor-by-`distance2` shape, kept deliberately extensible (add
an enum entry + one init block).

## STONE at the origin

`STONE` sits at the origin of noise space and the themed biomes are offset from
it (LUSH at temperature 0.3, humidity 0.3; CRYSTALS mirrored at -0.3, -0.3, so the
two never border each other directly — STONE always lies between). Because selection is nearest-neighbor,
the boundary between two biomes is the perpendicular bisector of their points, so
a biome's share of the cave band is set by how far and in which direction it is
offset — no explicit rarity threshold needed. Adding a themed biome means adding
another offset point; STONE keeps the middle.

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

## Per-voxel cave-air ownership

The biome's block effects (base block and skin) are classified on the fly inside
the fill loop and baked straight into the block choice. The base block covers
**all** solid stone with `y < caveNoiseMaxY` (not just cave walls), so exposed
faces anywhere in the band read as the biome. Cave structures read the biome once
per captured layer at fill time. Separately, chunks retain one byte per voxel for
carved cave air (`0xff` means non-cave); this lets the post-structure decorator
pass identify exact floor, wall, and ceiling cells across chunk boundaries without
retaining the generation noise fields.

## Secondary rock

`secondaryBaseBlock` / `secondarySkinFringeBlock` let a biome alternate between two
rock types (LUSH: stone and marble, each with its own overgrown fringe) on a
separate low-frequency coarse field (`fnCaveRock`, ~160-block features, thresholded
at `caveSecondaryRockThreshold`). It is sampled on the same downsampled grid as the
other cave fields, so the rock boundary is smooth and seam-free across chunks; the
skin and clay are rock-agnostic and lie on top of whichever rock is chosen.

## Slope-dependent surface rock

`flatSurfaceBlock` / `secondaryFlatSurfaceBlock` replace near-surface rock where the cave
surface is flat-ish, so CRYSTALS reads as basalt on steep walls and cracked basalt on
floors, ceilings and gentle slopes (its stone regions stay plain stone on every slope). The
surface normal is the gradient of the carve distance (`noise - caveSurfaceVal`) by central
differences of the cave noise grids; the y difference folds in the carve threshold's own y
dependence so the near-surface fade band does not read as a tilt. This is why the two
cave noise grids carry a one-block XZ margin: the x/z differences at a chunk border read
the neighbor chunk's first column, which is what keeps the classification seamless (about a
quarter more cave noise per chunk). Only rock within `caveFlatSurfaceShellDist` noise units
of the surface is classified, the same shell idea as the skin below.

The obvious cheaper alternative — mark voxels with air directly above or below — was
rejected because it would put the flat block on every exposed top face, so the base rock's
top texture (the columnar cross-section) could never show. With a true slope, a steep wall's
stair-step ledges stay base rock and display it.

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
