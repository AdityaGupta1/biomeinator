# LOD improvements

Follow-ups for the terrain LOD system on branch `claude/chunk-lods-f00a1b` (latest commit `4fd9eb19`).
Design and gotchas of what exists are in `knowledge/terrain/terrain_lod.md`; read it first.

## Current state

- Full-res chunks out to the BLAS distance (render distance + 1).
- Voxel ring out to `--lodVoxelDistance` (default 64 chunks): level 0-2 tiles built from surface-only
  chunks (`Chunk::generateSurfaceOnly`), downsampled 2x, meshed as boxes. Carries trees, structures,
  pillars and overhangs past the chunk distance.
- Beyond: smooth heightfield tiles from noise samples (`ChunkGenerator::sampleLodColumns`), up to 128
  cells per side, flat-shaded where steep.
- Water fade by camera distance in `water_waves.hlsli` has a TODO: the user isn't fully happy with it.

Known issues not covered below:
- Sea ice in heightfield tiles still shows a dark outline along shores after the any-corner water rule.
  Not yet diagnosed.
- Tianzi pillars in heightfield tiles are spiky (heightfields can't represent vertical walls). A 3D
  density isosurface (surface nets / dual contouring on the generator's density lattice) was discussed
  as the far representation that fixes this; deferred.

## Recommended order

1. Fog (biggest visual win for the least work)
2. Seam between full-res chunks and voxel tiles (cracks done; ledge later)
3. Decorator emulation
4. Per-chunk surface-only pipeline (ring generation speed)

## 1. Fog

Problem: far LODs are too visible, so their coarseness shows (spiky heightfield peaks, faceting). The
user likes the look where nearby detail is sharp and distant terrain is a hazy silhouette (reference:
the Minecraft shader / Distant Horizons screenshots they shared, with valley mist and aerial haze).

Current model (`src/shaders/light/fog_density.hlsli`, `computeFogSigmaS` in
`src/rendering/renderer/renderer.cpp`):
- One medium: density peaks at sea level (linear ramp from 24 blocks below), exponential falloff above
  with `--fogScaleHeight` 40.
- Strength `fogPeakSigmaS` 0.004, full only within 30 s of sunrise/sunset and zero from 120 s away. At
  midday there is no fog at all, which is when far LODs look worst.
- Marched with sun shadow rays per step (`computeFogInScatter` in `fog.hlsli`, 8 steps).
- Voxel bounds (used for miss-ray fog distance) already cover the LOD area when LODs are on.

Proposal: two components, constant through the day (optionally a small dawn/dusk boost on top):
- Aerial haze: thin and nearly height-independent (very large scale height), so distance alone fades
  terrain, roughly half extinction at 2-4k blocks (sigma around 2e-4 per block; tune). Light it
  analytically (sun phase + sky ambient, no shadow rays): over kilometre-long segments a few shadow
  tested steps would be noisy, and occlusion matters little at that scale.
- Ground mist: dense with a small scale height, concentrated near and below a low altitude (sea level
  and valleys). Keep the shadow-tested march for it, which gives the god rays.
- Fog is voxel mode only, and goldens include fog, so changing it means regolding.

## 2. Seam between full-res chunks and voxel tiles

Done: the bright crack lines are gone. Voxel tiles cull their outer faces against the real blocks
beside them instead of the rounded-up margin cells (see `knowledge/terrain/terrain_lod.md`).

To revisit later: the ledge of up to one block where voxel tiles sit above the chunks beside them,
since downsampling rounds surfaces up. Majority rounding is not the fix on its own: it would crack the
seam from the chunk side. Candidate: mesh each tile's outermost 2-block strip at full resolution from
the real blocks, so the step moves inside the tile where culling is exact.

## 3. Decorator emulation

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

## 4. Per-chunk surface-only pipeline

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
