_Last edited: 2026-10-07_

# Terrain LODs

`src/terrain/terrain_lod.h/cpp` shows terrain out to `--lodDistance` chunks as smooth heightfield tiles, and
`ChunkGenerator::sampleLodColumns` samples their columns. Automated runs default `--lodDistance` to 0, so goldens
and perf baselines don't see them unless it is passed.

## Data comes from the noise, not from chunks

A tile samples the generator's noise at its own cell spacing instead of downsampling generated chunks.
Full chunk generation is the expensive part of streaming and evicted chunks are never stored, so
building distant terrain from chunks would mean generating the whole LOD area at full detail. The
cost is that tiles only know what the noise knows before any chunk pass runs: no detail noise, caves,
structures, decorators or swamp/oasis water shaping. Surface blocks follow the chunk rules (grass
resolution, shore band, snow cap and its steepness test, topsoil depth, sea ice, snow layers) through
helpers the chunk generator shares, so the two agree at the seam. Snow layers skip the hollowness bias,
which needs finer heights than coarse cells have.

## Smooth surface, block materials

Tiles are a grid with one shared vertex per sample, at the sub-block height where the density crosses
zero (as chunks' `terrainSurfaceHeight`), not boxes per block. Stepped boxes left a sub-pixel staircase of
edges and dark walls on every distant slope, which aliased badly, and cost about six times the geometry.
Each cell splits along its flatter diagonal so ridges and valleys stay creased along their length.
Gentle cells share smooth vertex normals; steep cells are faceted with their triangles' own normals. A
heightfield turns a cliff into long thin triangles, and a smooth normal averaged with the ground above
and below strays so far from theirs that it streaked their shading and shadows into spikes.

Landforms built from vertical walls opt in to cliff columns (`BiomeData::lodCliffColumns`, Tianzi): there
a cell dropping more than a couple of cell widths is four flat quadrants at its corners' heights with
vertical walls between them, so walls stand halfway between samples and pillars keep flat tops. Sloped
triangles spanning a pillar's foot to its top made every Tianzi pillar a spike. It is per biome rather
than by slope because the same drop is a cone on quartz spikes and a ramp on mesa walls, which columns
turned into rectangles and stair patches set into smooth slopes. Where a column cell meets a sloped one,
slivers fill the gap between the flat quadrant tops and the neighbor's straight edge.

A cell still shows one block, chosen as block terrain at that slope would mostly look: its top block
where the slope is gentle (under 45 degrees, where block terrain shows more top than side); where it is
steeper, the side of its top block if the cell drops no more than the topsoil depth (snowy grass under a
snow layer, so snowy slopes read white as they do in chunks), else rock. Rock comes from
`LodRockStrata`, sampled by height on a world-aligned grid as fine as the cells: landform rock is banded
by height (Mesa terracotta, Tianzi strata), and taking each column's rock at its own top turned the bands
into vertical stripes across columns.

The broad terrain noise is sampled on the same 4-block world lattice chunks use and interpolated the
same way, so a one-block cell's height equals its chunk's wherever the chunk has no detail noise or
surface cave. That keeps the seam with full-resolution chunks small for the near tiles.

## Voxel tiles near the chunks

Within `--lodVoxelDistanceScale` times the render distance (2 by default), tiles are subdivided down to `maxVoxelTileLevel` and built from real chunks
instead of the noise, downsampled 2x into voxels. That carries trees, structures, pillars and overhangs
past the chunk distance, where heightfields dropped them at a hard edge.

- Surface-only chunks skip what can't be seen from afar, which is most of terrain generation's cost:
  cave shape and cave biome noise, cave structures, decorators, and filling rock far below the
  surface. Structures and snow layers still run, so placements match the full chunks they become.
- They live outside the region pipeline (null region) and never report state changes, so the terrain
  manager never sees them. `SurfaceChunkCache` generates them a task per chunk and step instead: terrain,
  then the structure pass once the structure neighborhood has terrain, then downsampling into cells. Full
  blocks are freed as soon as no waiting neighbor needs them, leaving a few KB of cells per chunk, which
  tiles mesh from once their chunks and the ring around them are ready.
- Generating a tile's chunks and their structure margin inside the tile's own task, as an earlier version
  did, was serial, regenerated margins (9 chunks for a level-0 tile's 1), spent half its time finding the
  height band, and held so much memory that only four could run: workers sat at 12% while the ring lagged.
  Per chunk, the useful work is about a millisecond.
- A chunk regenerated for a neighbor's structure pass after its own cells exist is only read for its
  immutable terrain, so its own structure pass never runs twice. Cells are dropped only once their chunk
  holds no terrain, and a chunk's cells are never regenerated while its old terrain lives.
- Only the highest-priority request may exceed the terrain budget, so neighborhoods that later requests
  left half generated can't stall every request.
- Regeneration, not per-chunk work, is what the ring costs: each chunk's terrain is needed by up to nine
  structure passes requested at different times. Freeing terrain as soon as no waiting chunk claimed it
  generated each chunk's terrain 4-5 times, and dropping cells after a timeout regenerated them when the
  camera passed and the chunks left the chunk distance behind it. So:
  - Cells are kept by distance, over the whole area out to the ring's edge and a quarter of that again
    past it, so a camera turning back finds them. Without the margin a turning walk built each chunk's
    cells 1.66 times; with it, 1.05, for about 13% more cells memory (cells are about 9 KB a chunk).
  - Unused terrain is kept in a least recently used queue under a byte budget. Surface-only terrain is
    compacted after its terrain pass: a column is a few runs of blocks (5.5 on average, against 512
    blocks), and its masks differ from all-air, all-solid-cube or neither in about one 64-block word. So a
    chunk waits for its own structure pass in about 17 KB instead of about 290 KB, and once downsampled
    keeps only the compacted masks, heights and structures its neighbors read. The blocks are expanded
    into a pooled full array for the structure pass. Most regeneration was edge-of-ring terrain generated
    only as a neighbor and evicted with its blocks before its own cells task ran; with blocks compacted,
    the same memory holds over ten times as many waiting chunks.
  - Requests are ordered by whole priority steps and then around the camera, so a chunk's neighbors are
    requested soon after it, except that requests whose terrain already holds blocks go first: they need
    no terrain generated, and running them gives their blocks back. Ordering by how much of each
    neighborhood had terrain (a slow flood fill) did no better.
  
  Together these took a 40 blocks/s turning walk from 4.6 terrain generations per chunk to 1.0 (1.03 per
  cells task) with a 256 MB budget, the memory the uncompacted caps used for 1.26. Compaction is a linear
  pass per chunk on the workers and didn't change frame times or stutter.
- A request claims its neighborhood before freeing unused terrain to make room, or it could free the
  very terrain it was about to use.
- Tasks only start once an update, so the cap on tasks in flight must cover a frame of work for every
  worker. At 64 a fresh load's ring kept two of 23 workers busy and took 15 s; at 512, 6.5 s.
- Placeholders inside the chunk distance start above the voxel levels: voxel tiles there would be
  replaced by chunks almost as soon as they were built.
- Downsampling keeps the most common block that fills from the bottom (any shape but plants and models),
  ties to the higher, so canopies, trunks and thin pillars survive (slightly thickened) and surfaces keep
  their top block. Plants are left out of cells and kept per cell column instead (see below).
- A cell is not a cube: it fills from its bottom to its highest column, in eighths of a block, counting a
  block in the upper row as standing on a full one. Shapes keep their heights that way (a snow layer on
  the ground is an eighth of a block thick, not a full cell of snow), which works for any shape anchored
  at the bottom of its block but could not represent top slabs or stairs. Solid cells cull against each
  other by comparing fills, as chunks compare shape heights. The cell's sides show its block and its top
  shows the tallest column's top block, preferring whole blocks for the former: otherwise a snow layer on
  leaves or grass, winning the tie, turned the whole cell into snow.
- A cell covers only the bounds of the columns holding its blocks (its footprint): otherwise one-block
  trunks, cacti and pillars doubled in width at their full height and trees looked squat. A neighbor hides
  a face only if its footprint is full, and a narrowed cell's inner sides always show; diagonal or L-shaped
  footprints keep the whole cell, so it is still one box per cell. Columns holding only a different cutout
  block don't count: the cell shows only its own block, so the leaves around a redwood trunk widened its
  log box to the whole cell. A cell's top shows the most common of its columns' top blocks, not the
  highest one's, or a trunk poking above each tier of leaves stamped a log top over the tier.
- Taking the highest column means tiles cover at least the real blocks. Faces out of a tile are culled
  against the real blocks beside it (each chunk keeps its four outer block slices for this), not the
  neighbors' cells, which cover more and would hide faces where the neighboring chunks are air, cracking
  the seam. Fills below the real blocks (e.g. by majority) would
  crack it from the other side: chunks cull their edge faces against real neighbors the tile no longer
  covers. What is left at the seam is the horizontal widening, up to a block.
- A partial cell holding water under its top also shows the water surface, since the cell above has
  none to show.
- Each chunk's cells cover only its own height band, with solid rock below. A tile meshes each chunk from
  the lowest band among it and its four neighbors, or a cliff wall facing a lower neighbor would be left
  out.
- Leaves keep their cutout, without OMMs, so a tile with leaf faces runs the anyhit alpha test. With
  OMMs on, a cutout texture's lower mips are opaque (OMMs test the finest one), so LOD geometry tests
  cutouts against mip 0 in the anyhit; at the lower mips distant LOD plants drew as solid cards, darker than
  the chunks beside them.
- Surface-only chunks run the floor decorator pass (not the cave one), and each cell column records its
  plants: which blocks hold one, the most common one and their highest base. Tiles show them as one or two
  axis-aligned cards through the column's center, whose position-derived UVs repeat the plant once per
  block, so a 2-block card is two plants: a quarter of the quads of per-plant crossed pairs. The number of
  plants shown was calibrated against chunks by average screen color: a flat card covers less than a crossed
  pair, so cards show about a quarter more plants than the column has. Only the most common plant shows, so
  lone flowers in grass are lost.

## Quadtree and selection

A tile at level L covers 2^L × 2^L chunks with at most 256 cells per side, so cells double each level
after level 4, and are never finer than voxel tiles' 2-block cells. One-block cells on the level-4 tiles
just past the voxel ring showed more detail than the voxel tiles that replace them, most visibly on cliff
columns. At 128 cells, a cell covered 3-6 pixels at 1440p where its level
starts, enough to show faceting; 256 brings it near a pixel for about 1.2 GB more VRAM at render
distance 30 (BLAS, indices and vertices of the heightfield tiles) and no measurable frame time. A tile subdivides when the camera is within two of its widths,
which keeps a cell's angular size roughly constant, or when it reaches into the chunk distance (the
BLAS distance, since every chunk with a BLAS can be shown); a level-0 tile's only child is its chunk.
Selection is by distance on the CPU rather than screen-space error: secondary rays see terrain behind
the camera, and the TLAS has to be chosen before tracing.

## Swaps are atomic, so tiles own chunk visibility

A tile is replaced by its four children (or a level-0 tile by its chunk) only in the frame all of them
have BLASes, and keeps showing until then. The two never overlap: a ray leaving full-resolution terrain
would otherwise hit the coarse surface under it and shadow itself, and screen-space compositing like
the Minecraft LOD mods use doesn't exist for secondary rays. With LODs on, `Terrain` therefore never
shows chunks itself; `TerrainLod::update` does, after the frame's chunk destruction so a chunk that lost
its instances is already not ready.

The update checks every chunk within the chunk distance each frame, and reading each chunk and its
instances cost more than the rest of the walk. Regions therefore cache which chunks were found ready, so
checking them again reads no chunk, and the walk finds a level-5 tile's region (tiles at level 5 and below
lie within one) once for all of its chunks. BLAS builds don't notify chunks, so a bit is only set when a
check finds the chunk ready; it must be cleared wherever a chunk stops being ready, which today is only
`destroyInstances` and being marked for destruction. That took the walk from 0.9 to 0.3 ms at 40 blocks/s.

Any tile that can't be shown makes every ancestor show itself instead, up to the root, so a gap
anywhere replaces terrain right next to the camera with the coarsest level. Coverage therefore must not
lapse:

- When the camera moves away and a tile stops subdividing before its own geometry exists, it counts and
  keeps showing whatever finer tiles or chunks still cover it (`areChildrenCoveredByExisting`); those
  are kept alive by being visited, not by being needed.
- Tiles near the chunk distance's edge keep their geometry, because the chunks covering them leave the
  BLAS distance after a single crossing, sooner than a tile could be generated.
- That margin alone doesn't keep up while moving fast: voxel tiles generate a few at a time, and the
  level-0 and level-1 ring around the chunk distance is over a thousand tiles. So chunks leaving the
  BLAS distance keep their instances while `TerrainLod` still shows them (the existing-coverage path
  treats them like any finer cover), and `Terrain` frees them once the tiles covering them take over.
  Freeing them on leaving made a level 4-6 ancestor, reaching back to the camera, stand in behind it.
- Generation is ordered by distance over level, as Distant Horizons does, coarser first on ties. Coarse
  tiles still cover the area first, but tiles next to the chunks don't wait on the whole horizon.
  Ordering by distance in tile widths did, so their coarse ancestors stood in right by the camera.

## Tiles inside the chunk distance

Tiles wholly inside the chunk distance exist only as placeholders while their chunks load. Below level
2 there are too many to be worth generating, and once all of a placeholder's children are ready its
geometry is freed, except within `keepGeometryMarginChunks` of the edge (see above).

## Gotchas

- Tile edges carry skirts four cells deep, so where a neighbor at another level meets the edge at a
  different height there is no gap to see through. Stitching the edges exactly would mean remeshing a
  tile whenever a neighbor changes level.
- Tiles have their own 12-byte vertex format (`PackedLodTerrainVertex`), sharing the packed terrain vertex
  buffer: tiles are far wider than the packed terrain vertex's local range. UVs are derived from
  position rather than stored, since textures repeat once per block on world-aligned axes. The projection
  follows each triangle's own normal (`setLodTerrainUvs`), not the vertices' smooth normals, which could
  pick different projections at one triangle's corners. Faces showing a block's side carry
  `FACE_FLAG_SIDE_PROJECTION` and always project horizontally: a cell picks its material from its average
  slope, and projecting by each triangle's slope instead laid side textures flat on cells near the
  threshold, turning the grass edge sideways.
- No tile keeps indices resident. Voxel tiles and all LOD water are quads of four fresh verts, so they
  use implicit quad indices like chunks. A heightfield face records its first vert and a pattern in
  `PerFaceData`: a cell of the shared corner grid (either diagonal), four verts of its own, or six. The
  indices the BLAS build reads are generated from those records, so the two can't disagree. Faces from
  the shared `addFace`/`addBoxFace` helpers get their record afterwards, from a cursor over the verts
  past the corner grid, which relies on each face's own verts being added in face order. The decode
  lives only in the closest hit (`getClosestHitTriangleVertexIndices`), which is safe because
  heightfields are opaque and never emissive: in `getTriangleVertexIndices`, which the raygen inlines
  into its inline-anyhit shadow loop and light sampling, it cost about 8% of path tracing in a
  LOD-heavy view even though those paths never decode a heightfield.
- The biome tint map only covers the render distance, so tiles bake each corner's biome tint into its
  vertices. The closest-hit shader interpolates it into `HitInfo::packedVertexTint`, which overrides
  the map for tinted faces.
- Tiles are never emissive. That keeps them out of the area-light structures, whose bounds assume
  everything lies within the render distance.
- LOD water is a static top surface with no walls, covering every cell with any corner underwater so it
  reaches the shore; terrain above the water level shows through it. A cell taking water only from its
  own sample left the slope below the waterline uncovered along every shore, which showed as a dark
  outline around ice. Sea ice is a half-block slab over the cell's floor, with edges only over open water;
  drawing it as a column down to the floor showed through the clear water beside it as ice pillars. Half a
  block, as the terrain surface sits, because dry terrain never dips below that, so the shore side of a
  slab, which has no face, never opens a gap under it.
- A cell holding water picks its material from its highest corner and the slope above the water. Judged
  down to the lakebed, every shore cell was steep and deep, so the shore above the water showed as rock,
  outlining lakes and ice.
- Blocks with `lodSideShowsBottom` (grass and similar) show their bottom texture on LOD side faces: the
  side texture's strip of the top aliased into stripes on distant slopes.
- Automated runs wait for `TerrainLod::isSettled` too, so `--renderToFile` with `--lodDistance` captures
  the finished LODs. `--fovY` reproduces a capture taken with zoom held (0.3 times the default 35).
- Known and deferred: while the world loads, with coarse tiles still standing in near the camera, a thin
  bright line can show across open water, likely where water surfaces of tiles at different levels meet.
  It is gone once the tiles settle and hasn't been reproduced in a steady state.
- With LODs on, the voxel bounds that water absorption and fog use for rays that miss everything cover
  the root tiles, not just the render distance. Underwater surfaces in LOD tiles otherwise got no
  absorption on their sky light, which showed as a line in the water at the render distance.
