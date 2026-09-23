_Last edited: 2026-09-22_

# Chunk Generator

`src/terrain/chunk_generator.h/cpp` — initializes FastNoise2 noise graphs. The actual generation runs in `Chunk::fillTerrainBlocksAndCreateStructures()`.

## Noise Architecture

FastNoise2 node graphs provide the five 2D surface biome axes, swamp warps and shore variation,
four 3D shape fields (broad terrain, fine terrain detail and two cave sources), and five coarse cave biome/material
fields. A random `noiseOffsetXZ` derived from the world seed shifts all sample positions so
different seeds produce different terrain even though node seed offsets are hardcoded.

## Shape Noise Sampling

The broad terrain shape is sampled every four blocks; both cave shapes every two blocks; fine
terrain detail every two blocks horizontally and four vertically, since that field is already
stretched vertically. All are then trilinearly reconstructed into the existing voxel grids before thresholding. Noise feature
scales, octave counts, and biome fields remain independent of these sampling spacings. The
finer cave spacing retains narrow passages and limits changes to cave-surface material gradients.

Fine detail is generated only in chunks touched by Mesa coverage or the Tianzi landform weight,
using smooth climate/erosion masks rather than jittered labels. Mesa detail is texture, so it
covers the whole label like Mesa roughness; Tianzi's belongs to its formations. Red desert and ordinary biomes receive none.
Its three octaves
span roughly four to sixteen blocks horizontally, with longer vertical features to limit detached
fragments, breaking up otherwise extruded cliff walls. The displacement
is scaled by the natural surface gradient so it remains visible on steep faces, with a cap to
preserve narrow formation cores. The same world-space slope query serves Tianzi's topsoil mask.
Tianzi's steep faces allow a larger inward displacement for shallow undercuts; a separate
upward limit keeps that slope boost from making thin spikes above planted crowns. This limit
does not enlarge the conservative displacement bound used for allocating the noise grids.
Mesa additionally tapers detail on gentle ground and plateau tops, retaining some bumps while
leaving steep slopes at full strength. This uses the slope before fine displacement, so bumps
do not amplify themselves; apply the taper before computing the sampled Y bounds.
Pond and dam footprints suppress detail continuously to preserve water containment.

Reconstruction keeps world Y contiguous and shares each XZ interpolation across a coarse Y
interval. Keeping dense output grids lets carving, cave blending, and central differences use
their existing voxel samples without repeating interpolation in the fill loop.

All three world axes are snapped to a common lattice **before** adding the seed's XZ offset.
Snapping just the chunk origin is insufficient: cave grids start one block outside the chunk,
and the clipped terrain/simplex Y origins vary between chunks. Negative coordinates need floor
division. Interpolation endpoints enclose the requested range, including the one-block cave
gradient margin; overlapping requests must reconstruct the same world samples even when their
Y bands differ. A spacing of one uses the original direct grid-generation path for comparisons.

## Cave Noise Design

Two cave noise sources: **worley** (cellular, produces rounded tunnels) below `caveWorleyBoundFraction * terrainBaseHeight`, **simplex** (spaghetti-style) above `caveSimplexBoundFraction * terrainBaseHeight`. Between those bounds, blends through `min(worley, simplex)` at the midpoint — this avoids abrupt transitions and lets the more open of the two dominate in the overlap zone.

Both cave grids are generated with a one-block XZ margin (`caveNoiseSizeXZ`) so the
slope classification in [cave_biome_system.md](cave_biome_system.md) can take central
differences at chunk borders; per-voxel reads index by `caveColumnIdx`, not `columnIdx`.
Each noise source is only generated for its relevant y-range across the chunk (worley up to `max(terrainBaseHeight) * caveSimplexBoundFraction + 2`, simplex from `min(terrainBaseHeight) * caveWorleyBoundFraction - 2`), and both are capped at `caveMaxY`. This avoids generating noise where it will never be read.

Two mechanisms suppress caves near the surface:
- **Surface fade**: `caveSurfaceVal` ramps down approaching `terrainBaseHeight`, making the threshold harder to meet and closing caves near the terrain surface.
- **Altitude squash**: above y=240 an additive term on `caveSurfaceVal` smoothly closes caves so tall mountain peaks remain solid.

Quartz formation material is determined before this carve pass and bypasses it entirely. This
keeps the crystal solid and prevents cave-air metadata from placing decorations inside it.
Tianzi instead suppresses carving in the formation volume above its original ground height,
with a short seal fading into the roots. Rock and skin classification still run separately:
the user wants the exposed stone/marble patches at the transition, so suppressing the entire
cave-material pass would incorrectly repaint those areas. No cave-air markers or cave layers
may originate inside the solid pillar, while deeper cave systems remain available.

Lamp scatter is separate from that broad rock/skin classification and is off unless all of the
following hold: it is eligible only below both the local base height and the shared pre-formation ground by the
surface fade depth, outside the pillar, and near the final carve threshold including the
root seal. This preserves underground cave lighting without treating exposed mountain rock
or sealed formations as places to scatter lights. These tests use existing column and voxel
fields, so placement remains independent of chunk generation order.

## Cave Biome Noise

Two additional 3D fields (temperature, humidity) drive cave biome theming — see
[cave_biome_system.md](cave_biome_system.md). They are generated coarsely
(downsampled) over `[0, caveNoiseMaxY]` and trilinearly interpolated, then
biased by the column's 2D surface noise. Solid-voxel classification picks the block that
replaces `STONE` in the `STONE`/`LAMP` choice, for every solid voxel in the cave
band. Three more coarse fields (skin thickness, skin patch, secondary rock) drive
the optional per-biome surface skin and rock choice; the skin uses the carve noise
itself as its distance proxy — see [cave_biome_system.md](cave_biome_system.md).
For carved air, generation records only a cave-air bit and retains the two biome
fields plus surface biases. Classification waits until decoration finds an air
cell bordering terrain support; cave interiors never need a biome lookup.

## Heightfield Design

The terrain isn't a simple heightmap — it uses a 3D surface threshold (`terrainNoise < surfaceVal`) so overhangs can form. But the threshold is shaped by a per-column `terrainBaseHeight` and `terrainSurfaceMultiplier`:

- **Below base height**: the surface multiplier is doubled (`terrainBelowHeightfieldSurfaceMultiplier = 2`), which makes underground much more uniformly solid and flattens the base. Without this, you'd get as many air pockets below as above. The asymmetry is intentional (it was chosen because it looked better, not derived). Its side effect is that density amplitude (roughness) also raises the effective surface a little, since noise builds up above the base more easily than it carves below. Don't compensate for that bias; just keep roughness away from raw climate so the small shift stays smooth (see [biome_system.md](biome_system.md)).
- **Near coast** (`inland` near 0): base height is pulled toward `seaLevel + 8` via smoothstep, creating gentle shorelines rather than cliffs.
- **Relief and formations**: peak and erosion jointly control broad relief; smooth terrace
  shaping and a shared finite-support formation sampler supply plateaus, pillars and spires.
  These modify the same base height and surface amplitude before voxel thresholding, so
  transitions remain continuous across biome labels. See [terrain_profiles.md](terrain_profiles.md).

Natural terrain is independent of local water shaping. Swamp pond-height probes must evaluate
all five natural-terrain inputs and their world positions, including climate-dependent
formations. Oasis bowls then blend into this natural terrain, override local water levels,
and reuse the bounded cave-waterline seals.

## 3D Noise Bounds Optimization

The 3D terrain noise is only sampled in the Y range that could possibly contain the surface
(derived from `surfaceValBound / multiplier`, widened on both sides by the maximum fine detail
displacement). Fine noise is explicitly clamped to its assumed bound. Leaving out that extra
displacement would truncate outcrops at chunk-dependent heights. For flat biomes this might be
a 30-block band; for mountains it's larger. This avoids sampling trivially solid or empty voxels.

## Structure Creation Happens Here

After blocks are filled, structure candidates are generated using the heightfield (which is in scratch memory and would be lost after this task) and biome data. See [structure_system.md](structure_system.md) for the placement algorithm.

Tianzi also scans actual planted surfaces for side shelves below the highest voxel; soil there
is limited to exposed formation stone above the shared ground. Exposed-surface structure
candidates come from the same kind of column-local scan, but their fit and spacing are resolved
later against neighbors' terrain masks (see [structure_system.md](structure_system.md)).
