_Last edited: 2026-05-24_

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
cave biome to whatever is above it (a desert tends toward BRIMSTONE below) while
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

The biome's only current effect is the base block, so it is classified on the
fly inside the fill loop and baked straight into the block choice — nothing is
stored per voxel. Theming covers **all** solid stone with `y < caveNoiseMaxY`
(not just cave walls), so exposed faces anywhere in the band read as the biome.
A per-voxel/per-region `CaveBiome` store will only be needed once cave structures
or decorators land.
