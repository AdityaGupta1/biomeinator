_Last edited: 2026-10-04_

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
- Only the highest-priority request may exceed the cap on chunks holding terrain, so neighborhoods that
  later requests left half generated can't stall every request.
- Regeneration, not per-chunk work, is what the ring costs: each chunk's terrain is needed by up to nine
  structure passes requested at different times. Freeing terrain as soon as no waiting chunk claimed it
  generated each chunk's terrain 4-5 times, and dropping cells after a timeout regenerated them when the
  camera passed and the chunks left the chunk distance behind it. So:
  - Cells are kept by distance, over the whole area out to the ring's edge and a quarter of that again
    past it, so a camera turning back finds them. Without the margin a turning walk built each chunk's
    cells 1.66 times; with it, 1.05, for about 13% more cells memory (cells are about 9 KB a chunk).
  - Unused terrain is kept in least recently used queues. A neighbor's structure pass reads only a
    chunk's masks, heights and structures, never its blocks, so a downsampled chunk gives its blocks back
    and stays cached at about 35 KB instead of 300 KB. Compact terrain and terrain still holding blocks
    have separate caps and queues: with one queue, freeing room for blocks discarded compact terrain first
    and barely helped.
  - Requests are ordered by whole priority steps and then around the camera, so a chunk's neighbors are
    requested soon after it.
  
  Together these took a 40 blocks/s turning walk from 4.6 terrain generations per chunk to 1.4. What is
  left is about 1.36 per cells task, little of it from caps or released claims: quadrupling the compact
  cap took it to 1.29 and the with-blocks cap to 1.28, and releasing claims of unrequested chunks frees
  almost nothing. Most of it is terrain generated as a neighbor and evicted with its blocks before its own
  cells task runs, which then needs the blocks again.
- A request claims its neighborhood before freeing unused terrain to make room, or it could free the
  very terrain it was about to use.
- Tasks only start once an update, so the cap on tasks in flight must cover a frame of work for every
  worker. At 64 a fresh load's ring kept two of 23 workers busy and took 15 s; at 512, 6.5 s.
- Placeholders inside the chunk distance start above the voxel levels: voxel tiles there would be
  replaced by chunks almost as soon as they were built.
- Downsampling keeps the most common block that fills from the bottom (any shape but plants and models),
  ties to the higher, so canopies, trunks and thin pillars survive (slightly thickened) and surfaces keep
  their top block. Plants vanish.
- A cell is not a cube: it fills from its bottom to its highest column, in eighths of a block, counting a
  block in the upper row as standing on a full one. Shapes keep their heights that way (a snow layer on
  the ground is an eighth of a block thick, not a full cell of snow), which works for any shape anchored
  at the bottom of its block but could not represent top slabs or stairs. Solid cells cull against each
  other by comparing fills, as chunks compare shape heights. The cell's sides show its block and its top
  shows the tallest column's top block, preferring whole blocks for the former: otherwise a snow layer on
  leaves or grass, winning the tie, turned the whole cell into snow.
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
- Leaves keep their cutout, without OMMs, so a tile with leaf faces runs the anyhit alpha test.

## Quadtree and selection

A tile at level L covers 2^L × 2^L chunks with at most 256 cells per side, so cells are one block up to
level 4 and double each level after. At 128 cells, a cell covered 3-6 pixels at 1440p where its level
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
- With LODs on, the voxel bounds that water absorption and fog use for rays that miss everything cover
  the root tiles, not just the render distance. Underwater surfaces in LOD tiles otherwise got no
  absorption on their sky light, which showed as a line in the water at the render distance.
