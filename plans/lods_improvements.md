# LOD improvements

Follow-ups for the terrain LOD system on branch `claude/chunk-lods-f00a1b` (draft PR
AdityaGupta1/biomeinator#424). Design and gotchas of what exists are in `knowledge/terrain/terrain_lod.md`;
read it first. This file tracks what is left, what was tried, and how to work on it.

## Current state

- Full-res chunks out to the BLAS distance (render distance + 1). Chunks leaving it linger until the LOD
  tiles covering them are ready, so coarse tiles never pop in next to the player.
- Voxel ring out to `--lodVoxelDistanceScale` times the render distance (default 2): level 0-2 tiles meshed
  from `SurfaceChunkCache`.
  - Surface-only chunks are generated a task per chunk, with structures and floor decorators, and
    downsampled 2x2x2 into cells that keep block shape heights (snow layers), cover only the columns
    holding their blocks (footprints, so trunks and cacti stay thin), and top with their columns' most
    common top block.
  - Plants show as axis-aligned alpha-tested cards per cell column, calibrated against chunks by average
    screen color. LOD cutouts test mip 0 in the anyhit, as OMMs do for chunks.
  - The cache keeps cells by distance and unused terrain in one LRU queue under a 256 MB byte budget;
    surface-only terrain is compacted (block runs per column, one stored mask word per column), about
    17 KB a chunk. A turning walk generates each chunk's terrain about once.
- Beyond: smooth heightfield tiles from noise samples (`ChunkGenerator::sampleLodColumns`), up to 256 cells
  per side and never finer than voxel cells (2 blocks). Steep cells are faceted; Tianzi cliffs
  (`BiomeData::lodCliffColumns`) are flat-topped columns with vertical walls.
- Aerial haze fades distant terrain toward a desaturated zenith sky color (`knowledge/shaders/path_tracing.md`).
  Fog is on all day with the user's tuned defaults.
- The LOD update costs about 0.6 ms per frame standing, 0.8 ms at 40 blocks/s and 1.3 ms at 100 (render
  distance 30); regions cache which chunks are ready so the walk reads no chunk for most of them.

Done in this round, all committed and pushed: seam cracks at the voxel ring, constant fog, aerial haze,
coarse tiles popping in near the player (lingering chunks), snow layers and block shapes in voxel tiles,
ring generation speed, the voxel distance scaling with render distance, the sea ice outline and grass sides
on LOD tiles, water shading fade distances, Tianzi spikes, the heightfield/voxel resolution mismatch,
decorators on voxel tiles, cell footprints, redwood trunk tops and widths, region readiness caching, and
the compacted terrain cache.

## Remaining work

1. Structure stand-ins and plant darkening on heightfield tiles (deferred by the user)
2. Voxel mode regolding
3. World import/export with LODs
4. Possible further speedups
5. Revisit the fog look

Known and accepted (the user decided not to fix these):
- The water seam seen only while the world loads (`knowledge/terrain/terrain_lod.md`, Gotchas).
- Only a cell's most common plant shows, so lone flowers in grass are lost: LOD tiles only need to look
  right in aggregate.
- Redwood canopies are slightly sparser around the trunk (a cell whose majority is log drops the leaves
  sharing it).
- Tianzi cliff walls show one rock block per cell, not strata bands, and steep-but-sloped Tianzi
  shoulders mesh as staircases (cells cross the 2-cell-width drop threshold over several cells).

### 1. Structure stand-ins and plant darkening on heightfield tiles

Heightfield tiles have neither structures nor plants, so forests end at the voxel ring's outer edge and
plant-covered ground is slightly brighter past it.

Structure stand-ins: default grid placement is a pure function of world position (one jittered candidate
per grid cell, filtered by biome, ground height, water and the treeline; `knowledge/terrain/structure_system.md`),
so heightfield tiles can compute the same candidates from their noise samples without generating chunks.
Only ledge-scanning placement (Tianzi) and real-block support/clearance checks are lost.
- Nearest heightfield level (5): proxy trees, a canopy box sized per structure type plus maybe a trunk,
  about 10-20 triangles each; estimated +40-70% triangles on forested level-5 tiles only.
- Level 6 and beyond: a canopy shell, raising forested cells by the biome's canopy height with a leaf
  material, so forests read as a lifted dark-green mass. No extra geometry.

Plant darkening, since plants there are under a pixel:
1. Coverage per (biome, ground block): summed weight of the `Decorator` entries that can stand on that
   block over the total weight including the `AIR` entry (e.g. grassland on grass blocks 14/29, about 48%;
   desert on sand 13/73, about 18%).
2. Per decorator, at texture load: mean color of its opaque texels (times the biome tint for tinted plants)
   and its opaque fraction. Mix entries by weight into one plant color per (biome, ground block).
3. Blend factor = coverage x opaque fraction x a viewing factor (about 0.6-0.8, tunable). New albedo =
   mix(ground, plant, blend) x (1 - shadow strength x coverage).
4. Bake it as a per-(biome, block) color multiplier into `PackedLodTerrainVertex::packedTint` at meshing;
   untinted tops (sand) need a face flag to apply it.
5. Calibrate against voxel tiles at the ring's edge by average screen color, as the plant cards were.

Tried or rejected for decorators:
- Statistical darkening on voxel tiles: replaced by real plant cards, which closed the seam with chunks.
- Composited multi-plant "clump" textures on crossed cards: prototyped in Python (local only, in the
  ignored `build/clump_prototype/`); bunched plants at cell centers and needed texture compositing. Cards with
  position-derived UVs repeat the existing plant textures instead.
- Cards showing exactly the cell's plant count were brighter than chunks (a flat card covers less than a
  crossed pair); about 1.25 plants per real plant matched within about 3%.
- DXR instancing per tuft: TLAS-level instancing only, hundreds of thousands of instances, tiny BLASes
  are the worst case for RT hardware.
- Procedural intersection shader over a thin layer above tiles: slowest RT path, grazing rays march many
  cells, can't match the chunks' sequential decorator RNG, still aliases.

### 2. Voxel mode regolding

The voxel goldens need regolding as a whole:
- Fog is on all day and goldens include fog (e.g. `grass_biome_blend` fails at 0.0145 against 0.01 from
  fog alone).
- Automated runs default `--lodDistance` to 0, so goldens don't see LODs. Decide whether goldens pass
  `--lodDistance` (likely, so they cover LODs); then nearly every voxel scene changes (distant terrain, the
  ring, fog and shadows past the chunk distance). Regold all voxel tests in one pass once the LOD look
  settles, rather than per change. Automated runs already wait for `TerrainLod::isSettled` when LODs are on.

### 3. World import/export with LODs

Not yet checked with LODs on. Things to verify:
- Imported regions are never evicted and may come from an older build that generates differently, while
  LOD tiles (heightfield and surface-only) generate from the seed. Tiles beyond the imported area, and
  surface-only chunks next to it, may not match the imported terrain at the seam.
- Imported chunks get geometry through the normal pipeline; check that LOD tiles inside the imported area
  hand over to them (atomic swaps, lingering chunks) and that nothing assumes generated chunks.
- Reimport (`resetTerrainState`) resets `TerrainLod` and `SurfaceChunkCache`; check no task, pinned cells,
  lingering chunk or compacted terrain survives it.
- Export writes completed regions only; make sure lingering chunks and LOD state don't leak into it.
- `pollAutomatedRunTerrain`'s imported-world path doesn't wait for LODs yet (only the procedural path calls
  `TerrainLod::isSettled`).

### 4. Possible further speedups

History: ring generation went from 15.4 s to about 6.5 s on a fresh load at render distance 30; the LOD
update from 3.9 ms to about 1.3 ms per frame at 100 blocks/s (regions cache chunk readiness); terrain
generations per chunk on a turning walk from 4.6 to 1.0 (cells kept a margin past the keep distance,
requests whose terrain holds blocks run first, compacted terrain under a byte budget).

Left on the table:
- Display in the LOD update (0.3-0.35 ms per frame) sorts and diffs the ~4000 displayed chunks and tiles
  every frame. Keeping unchanged areas out of the sort and diff is the largest remaining main-thread cost.
- Downsampling chunks the full-res pipeline already holds would skip surface-only generation near the
  chunk distance on a fresh load. Full chunks have caves, so their cells can differ slightly from
  surface-only neighbors, and the hook must run while the chunk's blocks are alive.
- Lingering chunks still pile up behind the camera at very high speeds (VRAM and TLAS entries); prioritize
  tiles that release them, cap them, or both.

Tried and dropped:
- Freezing deep level-3 tiles whose chunks were all ready, skipped until a per-8x8 readiness version
  changed: correct after one fix (a tile must freeze only when every chunk under it is ready, not when its
  children are covered, since deep placeholder tiles can cover with their own geometry), but only about 36
  tiles qualify at render distance 30 and it saved 0.08-0.24 ms.
- Ordering cache requests by how much of their neighborhood has terrain (a slow flood fill): no better than
  running requests whose own terrain holds blocks first.
- Raising count caps on uncompacted terrain: 1024 chunks holding blocks (256 MB) fixed fresh-load stalls
  (9.5 s to 7.0 s) but still regenerated 1.26 times per chunk; compaction reached 1.0 in the same memory.

### 5. Revisit the fog look

The user wants to revisit both the fog's god rays and the aerial haze: they look overly dark in some areas
and overly bright in others. Not yet investigated; start from captures of the problem areas (reproduce them
windowlessly as below). The fog's defaults (base sigma 0.0016, scale height 30, anisotropy 0.4, ambient
strength 0.7) and the haze's (half distance 3000, start 400, whiteness 0.5, brightness 2, sky band 0.05)
are all in the settings window's Atmosphere tab, so they can be tuned live. How each works is in
`knowledge/shaders/path_tracing.md`.

## Working on this

- The user runs the game to look: always `--voxelMode=true`, usually `--worldSeed=1738`. Commit and push
  only when asked each time. Measure before deciding; temporary instrumentation is marked `// TEMP` and
  removed before handing back.
- The user's screenshots show a Debug window with position, yaw, pitch and seed. Reproduce one without a
  window: `Biomeinator.exe --voxelMode=true --worldSeed=<seed> --lodDistance=512 --cameraX/Y/Z=<pos>
  --cameraYaw=<yaw> --cameraPitch=<pitch> --renderToFile=<path>.png`, plus `--fovY=10.5` when zoom (C) was
  held. To see which tile type draws a spot: `--lodVoxelDistanceScale=0` (heightfields only) or a lower
  `--renderDistance` (turns full-res chunks into voxel tiles), and `--debugColorChunks=true`.
- Perf walks: `--perfOutput=<json> --perfMoveSpeed=40 --perfMoveTurnFrames=300 --perfFrames=4000
  --worldSeed=100 --renderDistance=30 --lodDistance=512` (`knowledge/tests/perf_runs.md`). Compare CPU
  scope medians/p95 and per-frame CPU spikes from the report's `frames`; a ~110-140 ms BLAS-upload frame
  near frame 1300 appears in every run and is unrelated.
- Terrain regeneration was counted by temporarily logging each surface-only terrain and cells task with
  its chunk position and comparing counts per chunk.
- Changes to chunk readiness or cache state were validated with temporary per-frame checks against the
  real state (stale ready bits, frozen tiles, lossless compaction) on a 100 blocks/s walk with region
  eviction.
