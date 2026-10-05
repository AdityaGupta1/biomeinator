# LOD improvements

Follow-ups for the terrain LOD system on branch `claude/chunk-lods-f00a1b`.
Design and gotchas of what exists are in `knowledge/terrain/terrain_lod.md`; read it first.

## Current state

- Full-res chunks out to the BLAS distance (render distance + 1). Chunks leaving it linger until the LOD
  tiles covering them are ready.
- Voxel ring out to `--lodVoxelDistanceScale` times the render distance (default 2): level 0-2 tiles meshed from
  `SurfaceChunkCache`, which generates surface-only chunks a task per chunk, downsamples them into cells
  that keep block shape heights, and caches cells and compact neighbor terrain. Carries trees,
  structures, pillars and overhangs past the chunk distance.
- Beyond: smooth heightfield tiles from noise samples (`ChunkGenerator::sampleLodColumns`), up to 256
  cells per side, flat-shaded where steep.
- Fog is on all day with the user's tuned defaults (base sigma 0.0016, scale height 30, anisotropy 0.4,
  ambient strength 0.7).

Done in this round: seam cracks at the voxel ring, constant fog, coarse tiles popping in near the player,
snow layers and block shapes in voxel tiles, ring generation speed, the sea ice outline and grass sides
on LOD tiles, the water fade distances, and Tianzi spikes in heightfield tiles. The water seam seen while loading is deferred as known
(see `knowledge/terrain/terrain_lod.md`, Gotchas).

## Remaining work

1. Distance fog that fades far terrain to a neutral color (done: aerial haze, see
   `knowledge/shaders/path_tracing.md`)
2. Decorators in LOD tiles (deferred)
3. Voxel mode regolding
4. World import/export with LODs
5. Possible further speedups

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

### 2. Decorators in LOD tiles (deferred)

Symptom: dense grass/flower decorators on full-res chunks darken and texture the ground; surface-only
chunks skip decorators and 2x downsampling would drop them anyway, so voxel tiles (and heightfields)
look brighter and flatter. The difference is sharpest right at the full-res seam.

Leaning towards a hybrid: real decorators merged into the nearest voxel tiles (levels 0-1, just past the
chunks), statistical darkening beyond (level-2 voxel tiles and heightfields). At 1440p a block 500 blocks
away is about 3 pixels wide and about 1.5 at 1,000, so far tufts would only show as shimmering speckle.
Open questions: real decorators over the whole voxel ring or only near the seam; whether to accept the
anyhit alpha-test cost at first (LOD tiles have no OMMs) or reuse the chunks' decorator OMMs.

Real decorators, merged into tile geometry:
- Surface-only chunks already run the structure pass; the floor decorator pass could run in the same
  cells task (they skip it only because the cave half needs skipped data), so placement matches chunks.
- An X-shaped decorator is 4 triangles; merged in, roughly as many triangles as the voxel terrain
  itself, a few hundred MB for the ring. Fewer, larger tufts (e.g. 1 in 4 at 2x size) keep the coverage
  for fewer triangles.
- Cutouts run the anyhit alpha test without OMMs, on every ray type including shadows.

Statistical darkening, how it would be computed:
1. Coverage per (biome, ground block): summed weight of the `Decorator` entries that can stand on that
   block over the total weight including the `AIR` entry (e.g. grassland on grass blocks 14/29, about
   48%; desert on sand 13/73, about 18%). That is the expected value of the chunk decorator pass on flat
   open ground.
2. Per decorator, at texture load: mean color of its opaque texels (times the biome tint for tinted
   plants) and its opaque fraction. Mix entries by weight into one plant color per (biome, ground block).
3. Blend factor = coverage x opaque fraction x a viewing factor (tunable, about 0.6-0.8: LOD terrain is
   always seen at grazing angles, where X-shaped plants hide most of the ground). New albedo =
   mix(ground, plant, blend) x (1 - shadow strength x coverage) for the shadows between tufts.
4. Since the shader multiplies the top texture by the vertex tint, bake that as a per-(biome, block)
   color multiplier into `PackedLodTerrainVertex::packedTint` at meshing. Untinted tops (sand) need a
   face flag to apply the multiplier anyway.
5. Optional: sample the drift-patch noise at each vertex so flower patches show as color patches.
6. Calibrate the viewing factor and shadow strength against full-res chunks at the seam, by eye or by
   comparing average screen color.
Costs nothing per frame, no geometry, can't flicker; can't show individual tufts.

Rejected:
- DXR instancing: instancing exists only at the TLAS level, so each tuft would be a TLAS instance.
  Hundreds of thousands to a million instances against today's ~10,000 (about 64 B each plus nodes,
  100-150 MB, plus TLAS updates and deeper traversal), and tiny BLASes are the worst case for RT hardware
  (each instance entry transforms the ray and starts a new BLAS walk, for every ray type). Shared
  multi-tuft patches don't work on uneven terrain. NVIDIA's partitioned TLAS / cluster templates (RTX
  Mega Geometry) are built for this scale but are a large new dependency.
- Procedural intersection shader (an AABB over a thin layer above each tile, marching cells and
  intersecting hashed X-shaped quads): cheap to store, but intersection shaders are the slowest RT path,
  grazing rays march many cells, shadow rays must run it or skip it (the fog's occlusion query skips
  procedural primitives; skipping loses the tufts' shadows), hashed placement can't match the chunks'
  sequential decorator RNG at the seam, it needs its own hit group, and sub-3-pixel tufts still alias.
  It suits volumetric near-field effects (grass shells, fur), not LOD distances.

### 3. Voxel mode regolding

The voxel goldens need regolding as a whole:
- Fog is now on all day, and goldens include fog (e.g. `grass_biome_blend` fails at 0.0145 against 0.01
  from fog alone).
- Automated runs default `--lodDistance` to 0, so goldens don't see LODs yet. Once tests run with LODs,
  nearly every voxel scene changes: distant terrain, the ring, and fog and shadows reaching past the
  chunk distance. Decide whether goldens run with LODs on (likely, so
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
- Automated runs of imported worlds (goldens) once LODs are on there (see item 3).

### 5. Possible further speedups

Ring generation went from 15.4 s to about 6.5 s on a fresh load at render distance 30, and the LOD
update from 3.9 ms to 2.0 ms per frame while moving fast (see `knowledge/terrain/terrain_lod.md`).
Since then, regions cache which chunks are ready (the walk from 0.9 to 0.3 ms at 40 blocks/s, 1.05 to
0.54 ms at 100), and cells are kept a margin past the keep distance (terrain generations per chunk on a
turning walk from 2.2 to 1.4).
Left on the table:
- The rest of the walk (about 0.2-0.5 ms) still visits every tile and chunk each frame; chunks deep inside
  the chunk distance never change once ready, so that subtree's result could be kept.
- About 1.36 terrain generations per cells task, mostly neighbor terrain evicted with its blocks before
  its own cells task runs. Raising the with-blocks cap from 256 to 1024 only took it to 1.28.
- Downsampling chunks the full-res pipeline already holds would skip surface-only generation near the
  chunk distance on a fresh load.
- Lingering chunks still pile up behind the camera at very high speeds.
