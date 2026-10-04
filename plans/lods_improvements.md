# LOD improvements

Follow-ups for the terrain LOD system on branch `claude/chunk-lods-f00a1b`.
Design and gotchas of what exists are in `knowledge/terrain/terrain_lod.md`; read it first.

## Current state

- Full-res chunks out to the BLAS distance (render distance + 1).
- Voxel ring out to `--lodVoxelDistance` (default 64 chunks): level 0-2 tiles built from surface-only
  chunks (`Chunk::generateSurfaceOnly`), downsampled 2x, meshed as boxes. Carries trees, structures,
  pillars and overhangs past the chunk distance.
- Beyond: smooth heightfield tiles from noise samples (`ChunkGenerator::sampleLodColumns`), up to 128
  cells per side, flat-shaded where steep.

Done so far in this round:
- Seam cracks between full-res chunks and voxel tiles: voxel tiles cull their outer faces against the
  real blocks beside them instead of the rounded-up margin cells.
- Fog no longer fades with time of day; it is on all day with the user's tuned defaults (base sigma
  0.0016, scale height 30, anisotropy 0.4, ambient strength 0.7). Goldens include fog, so they need
  regolding.

## Remaining work

1. Coarse tiles popping in near the player (next up)
2. Distance fog that fades far terrain to a neutral color
3. Decorator emulation
4. Snow layers in voxel tiles
5. Per-chunk surface-only pipeline (ring generation speed)
6. Smaller items: seam ledge, sea ice outline, Tianzi spikes, water fade, dawn/dusk fog boost

### 1. Coarse tiles popping in near the player

Symptom: coarse LOD tiles still appear briefly near the camera, where everything around them is full-res
chunks. Earlier fixes (counting existing coverage in `areChildrenCoveredByExisting`, the keep-geometry
margin, distance / (level + 1) priority, `createBlasDistance` as the chunk distance) reduced it but did
not remove it.

Plan: reproduce with the perf random-walk runs, adding temporary checks that flag any LOD tile shown
near the camera whose chunks are all full-res and visible, then find the cause from the flagged cases.

### 2. Distance fog to a neutral color

Goal: far terrain fades into a neutral blue-whitish color, perhaps shifting slightly with time of day, so
distant LODs read as hazy silhouettes, while the existing ground fog and god rays stay as they are.

Tried and rejected: a separate aerial haze (constant density, half at 3k blocks) plus a thin ground mist
(scale height 10, then 25), replacing the existing fog.
- Colored by the horizon sky in the view direction, the haze made a warm orange band at the horizon,
  brightest sunward (the horizon sky is warm and dominated by the glow around the sun).
- Lit by sun phase plus zenith sky, it still looked bad.
- The thin mist lost the god rays over terrain well above sea level.

Direction for the next try: keep the existing fog untouched and add only the far fade, with an explicit
neutral color (blue-white, from a small set of settings or a gentle time-of-day curve, dimmed at night)
rather than one derived from the sky LUT. Fade by distance alone, with the fade start and strength as
GUI settings so it can be tuned live.

### 3. Decorator emulation

Symptom: dense grass/flower decorators on full-res chunks darken and texture the ground; surface-only
chunks skip decorators and 2x downsampling would drop them anyway, so voxel tiles (and heightfields)
look brighter and flatter.

Proposal: emulate statistically rather than placing decorators.
- At init, compute per biome the expected floor decorator coverage from its `Decorator` entries.
- Darken (and optionally shift) the baked biome vertex tint by coverage times a tuning factor that
  stands for the tufts' color and their small self-shadows. LOD vertices already carry a tint
  (`PackedLodTerrainVertex::packedTint`), applied to biome-tinted faces; untinted tops (sand etc.)
  would need a separate darkening path or a tint flag.
- Applies to both voxel and heightfield tiles, costs no generation, and can't flicker.
- Alternative considered: run the floor decorator pass in surface-only chunks (the cave decorator half
  is what needs skipped data). Not worth it: downsampling deletes them, and only their color matters.

### 4. Snow layers in voxel tiles

Symptom: snow layers turn into full-block snow in voxel tiles, so snowy ground sits up to a block too
high and looks lumpy.

Cause: `downsampleBlocks` counts `BlockShape::LAYER` as filling its cell, and voxel tiles mesh every
cell as a full box.

Fix: give a cell whose top is a snow layer a lowered top at the layer's actual height (the solid blocks
below plus `blockShapeTopHeight` of the layer), meshing its top face and the exposed parts of its sides
at that height. Side faces against a neighbor with a different top height need the same partial-height
treatment chunks give layers (`blockFaceVisible` compares top heights). Keep the cull-against-real-
blocks rule at tile edges consistent with the lowered tops.

### 5. Per-chunk surface-only pipeline

Why the ring fills slowly: each voxel tile task generates its chunks plus a one-chunk margin serially
(36 chunks for a level-2 tile's 16), and `maxGeneratingVoxelTiles` is 4, because each running tile
holds about 11 MB of chunk block buffers that the chunk buffer pool keeps forever. Most workers sit
idle, and margins are regenerated by every neighboring tile.

Options:
- Quick: raise the cap toward the worker count. Costs about 11 MB of permanently pooled buffers per
  extra concurrent tile, and each tile stays slow.
- Proper: generate surface-only chunks as their own per-chunk tasks with shared results:
  - Terrain per chunk once; structure pass per chunk once its 3x3 neighbors have terrain (the same
    dependency as the region pipeline).
  - Downsample each chunk once into compact cells (a band of 8x8xN cells, a few KB) and free its full
    block buffers right away.
  - Voxel tiles mesh from cached cells once all their chunks' cells exist; cells are evicted with the
    ring.
  This removes the duplicated margins and spreads the work across all workers. It needs dependency
  tracking, either a small dedicated scheduler or a surface-only mode in the region pipeline (chunks
  then switch mode by being discarded and regenerated when they come within the full-res distance).
- Measure first: time surface-only versus full chunk generation (CPU terrain benchmark harness,
  `knowledge/tests/cpu_terrain_benchmarks.md`) to size the ring.

### 6. Smaller items

- Seam ledge: voxel tiles sit up to a block above the chunks beside them, since downsampling rounds
  surfaces up. Majority rounding is not the fix on its own: it would crack the seam from the chunk side.
  Candidate: mesh each tile's outermost 2-block strip at full resolution from the real blocks, so the
  step moves inside the tile where culling is exact.
- Sea ice outline: sea ice in heightfield tiles still shows a dark outline along shores after the
  any-corner water rule. Not yet diagnosed.
- Tianzi spikes: heightfield tiles can't represent vertical walls, so Tianzi pillars come out spiky. A
  3D density isosurface (surface nets / dual contouring on the generator's density lattice) was
  discussed as the far representation that fixes this; deferred.
- Water fade: the distance fade of the water's shading detail in `water_waves.hlsli` has a TODO; the
  user isn't fully happy with it.
- Dawn/dusk fog boost: optional extra fog strength around sunrise and sunset on top of the constant fog.
