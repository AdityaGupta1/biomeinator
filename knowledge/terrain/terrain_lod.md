_Last edited: 2026-10-03_

# Terrain LODs

`src/terrain/terrain_lod.h/cpp` shows terrain out to `--lodDistance` chunks as heightfield tiles, and
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
which needs finer heights than coarse cells have. Cliffs show the topsoil's mid block down to the
topsoil depth and the landform rock below it; one rock for the whole cliff, so strata don't show.

The broad terrain noise is sampled on the same 4-block world lattice chunks use and interpolated the
same way, so a one-block cell's height equals its chunk's wherever the chunk has no detail noise or
surface cave. That keeps the seam with full-resolution chunks small for the near tiles.

## Quadtree and selection

A tile at level L covers 2^L × 2^L chunks with at most 64 cells per side, so cells are one block up to
level 2 and double each level after. A tile subdivides when the camera is within two of its widths,
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
- Generation is ordered by distance over level, as Distant Horizons does, coarser first on ties. Coarse
  tiles still cover the area first, but tiles next to the chunks don't wait on the whole horizon.
  Ordering by distance in tile widths did, so their coarse ancestors stood in right by the camera.

## Tiles inside the chunk distance

Tiles wholly inside the chunk distance exist only as placeholders while their chunks load. Below level
2 there are too many to be worth generating, and once all of a placeholder's children are ready its
geometry is freed, except within `keepGeometryMarginChunks` of the edge (see above).

## Gotchas

- Tile edges carry skirts four cells deep below the lower side, so a neighbor at another level never
  leaves a gap to see through. Inside a tile, cliffs stop at the lower neighbor.
- Tiles have their own 12-byte vertex format (`PackedLodTerrainVertex`), sharing the packed terrain vertex
  buffer: tiles are far wider than the packed terrain vertex's local range. UVs are derived from
  position and normal rather than stored, since textures repeat once per block on world-aligned axes.
- The biome tint map only covers the render distance, so tiles bake each corner's biome tint into its
  vertices. The closest-hit shader interpolates it into `HitInfo::packedVertexTint`, which overrides
  the map for tinted faces. Tinted tops are not merged into runs, since a run only carries the tints
  at its ends.
- Tiles are never emissive. That keeps them out of the area-light structures, whose bounds assume
  everything lies within the render distance.
- LOD water is a static top surface with no walls. Frozen sea is a solid ice column instead, so
  nothing shows under it.
- With LODs on, the voxel bounds that water absorption and fog use for rays that miss everything cover
  the root tiles, not just the render distance. Underwater surfaces in LOD tiles otherwise got no
  absorption on their sky light, which showed as a line in the water at the render distance.
