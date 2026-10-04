# LOD improvements

Follow-ups for the terrain LOD system on branch `claude/chunk-lods-f00a1b`.
Design and gotchas of what exists are in `knowledge/terrain/terrain_lod.md`; read it first.

## Current state

- Full-res chunks out to the BLAS distance (render distance + 1). Chunks leaving it linger until the LOD
  tiles covering them are ready.
- Voxel ring out to `--lodVoxelDistance` (default 64 chunks): level 0-2 tiles meshed from
  `SurfaceChunkCache`, which generates surface-only chunks a task per chunk, downsamples them into cells
  that keep block shape heights, and caches cells and compact neighbor terrain. Carries trees,
  structures, pillars and overhangs past the chunk distance.
- Beyond: smooth heightfield tiles from noise samples (`ChunkGenerator::sampleLodColumns`), up to 128
  cells per side, flat-shaded where steep.
- Fog is on all day with the user's tuned defaults (base sigma 0.0016, scale height 30, anisotropy 0.4,
  ambient strength 0.7).

Done in this round: seam cracks at the voxel ring, constant fog, coarse tiles popping in near the player,
snow layers and block shapes in voxel tiles, and ring generation speed (see `knowledge/terrain/terrain_lod.md`).

## Remaining work

1. Distance fog that fades far terrain to a neutral color (done: aerial haze, see
   `knowledge/shaders/path_tracing.md`)
2. Decorator emulation
3. Voxel mode regolding
4. World import/export with LODs
5. Smaller items: seam ledge, water seam, sea ice outline, Tianzi spikes, water fade, dawn/dusk fog boost
6. Possible further speedups

### 1. Distance fog to a neutral color

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

### 2. Decorator emulation

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

### 3. Voxel mode regolding

The voxel goldens need regolding as a whole:
- Fog is now on all day, and goldens include fog (e.g. `grass_biome_blend` fails at 0.0145 against 0.01
  from fog alone).
- LODs are off in headless runs today (`Terrain::lodsEnabled` checks `headless`), so goldens don't see
  them yet. Once tests run with LODs, nearly every voxel scene changes: distant terrain, the ring, and
  fog and shadows reaching past the chunk distance. Decide whether goldens run with LODs on (likely, so
  they cover them), then regold all voxel tests in one pass, after the LOD look settles, rather than per
  change.

### 4. World import/export with LODs

Not yet checked with LODs on. Things to verify:
- Imported regions are never evicted and may come from an older build that generates differently, while
  LOD tiles (heightfield and surface-only) generate from the seed. Tiles beyond the imported area, and
  surface-only chunks next to it, may not match the imported terrain at the seam.
- Imported chunks get geometry through the normal pipeline; check that LOD tiles inside the imported
  area hand over to them (atomic swaps, lingering chunks) and that nothing assumes generated chunks.
- Reimport (`resetTerrainState`) resets `TerrainLod` and `SurfaceChunkCache`; check no task, pinned
  cells or lingering chunk survives it.
- Export writes completed regions only; make sure lingering chunks and LOD state don't leak into it.
- Headless runs of imported worlds (goldens) once LODs are on there (see item 3).

### 5. Smaller items

- Seam ledge: vertical rounding is gone with the fill heights, but a cell takes its tallest column's
  height, so a step of up to a block remains where columns differ. Fills below the real blocks would crack
  the seam from the chunk side. Candidate: mesh each tile's outermost 2-block strip at full resolution
  from the real blocks, so the step moves inside the tile where culling is exact.
- Water seam: a thin bright line across open water near the camera, seen right after loading (a lake
  next to snowy shore). Possibly at a chunk/tile boundary in the water surface. Not yet investigated.
- Sea ice outline: sea ice in heightfield tiles still shows a dark outline along shores after the
  any-corner water rule. Not yet diagnosed.
- Tianzi spikes: heightfield tiles can't represent vertical walls, so Tianzi pillars come out spiky. A
  3D density isosurface (surface nets / dual contouring on the generator's density lattice) was
  discussed as the far representation that fixes this; deferred.
- Water fade: the distance fade of the water's shading detail in `water_waves.hlsli` has a TODO; the
  user isn't fully happy with it.
- Dawn/dusk fog boost: optional extra fog strength around sunrise and sunset on top of the constant fog.

### 6. Possible further speedups

Ring generation went from 15.4 s to about 6.5 s on a fresh load at render distance 30, and the LOD
update from 3.9 ms to 2.0 ms per frame while moving fast (see `knowledge/terrain/terrain_lod.md`).
Left on the table:
- The LOD update's tree walk (about 1.1 ms) checks every chunk's readiness each frame; chunks deep inside
  the chunk distance never change once ready, so that subtree's result could be kept.
- Terrain is still generated about 2.1 times per chunk on a turning walk (1.36 per cells task).
- Downsampling chunks the full-res pipeline already holds would skip surface-only generation near the
  chunk distance on a fresh load.
- Lingering chunks still pile up behind the camera at very high speeds.
