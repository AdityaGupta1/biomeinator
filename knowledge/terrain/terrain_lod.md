_Last edited: 2026-10-03_

# Terrain LODs

`src/terrain/terrain_lod.h/cpp` shows terrain out to `--lodDistance` chunks as smooth heightfield tiles, and
`ChunkGenerator::sampleLodColumns` samples their columns. LODs are off in headless runs, so goldens never
see them.

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

Within `--lodVoxelDistance`, tiles are subdivided down to `maxVoxelTileLevel` and built from real chunks
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

A tile at level L covers 2^L × 2^L chunks with at most 128 cells per side, so cells are one block up to
level 3 and double each level after. A tile subdivides when the camera is within two of its widths,
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
  outline around ice. Sea ice is a one-block slab over the cell's floor, with edges only over open water;
  drawing it as a column down to the floor showed through the clear water beside it as ice pillars.
- With LODs on, the voxel bounds that water absorption and fog use for rays that miss everything cover
  the root tiles, not just the render distance. Underwater surfaces in LOD tiles otherwise got no
  absorption on their sky light, which showed as a line in the water at the render distance.
