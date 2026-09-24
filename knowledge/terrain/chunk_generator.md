_Last edited: 2026-09-23_

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

Fine detail is generated only in chunks touched by the Mesa style weight or the Tianzi landform
weight, using smooth climate/erosion masks rather than jittered labels. Mesa detail is texture,
so it follows the same soft style weight as Mesa roughness; Tianzi's belongs to its formations. Red desert and ordinary biomes receive none.
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

## Snow Line

Snow capping runs in the top-block stamp, after the biome's `TopBlocks` are chosen, and
overrides them. It lives here rather than in [biome_system.md](biome_system.md) because it is an
altitude effect that cuts across biomes: relief raises ground everywhere, not only in
`MOUNTAINS`.

**The line comes from climate, not biome labels**, in keeping with the biome system's design rule.
Per column it is a base height plus:

- **temperature**, so cold columns whiten lower. Temperature is an unnormalised fbm sum (about
  ±0.7 at the 5th-95th percentile, ±1.1 at the extremes), so the very tallest hot peaks can still
  cap; that is intended.
- **aridity** (negative humidity), which raises it. Dry climates' real snow lines sit far higher,
  and this is what keeps hot, dry Mesa and red desert highlands essentially bare without naming
  them. A steeper temperature term was tried instead and made things worse: it lowered the line
  in cold forest and tundra more than it raised it in the warm regimes.
- **a dedicated low-frequency 2D field** (`fnSnowLine`) for wander. Reusing the biome noise would
  tie the snow boundary to biome boundaries and bring back the isosurface look.

**Tianzi is the one exception climate cannot express.** It is humid, spans cool to warm, and its
towers clear the line by 150+ blocks even in warm columns, where snow would erase its planted
summits. Its *landform* weight lifts the line out of reach, faded out with falling temperature so
only genuinely cold karst gets snowy tower tops. Landform weight, not style or coverage: it is the
weight that raises the towers, so the lift tracks exactly how much tower a column has. Style weight
extends past the label and was measured stripping snow from ~2% of the mountains biome next to
Tianzi; landform weight is zero outside the label.

**The base is calibrated against measured terrain, and depends on relief heights.** It was
chosen by sweeping `computeNaturalTerrain` over a large area on several seeds and counting each
biome's land above the line: roughly 40-60% of mountains, ~1% of forest, 1-3% of tundra, and a
few percent at most of Tianzi (cold edges), red desert and Mesa. #400's taller relief moved the
mountains figure from about half to ~90% at the old base, so re-measure whenever relief changes.
`baseHeight` is not the exact surface (3D noise still moves it), so treat the numbers as relative.

Constraints worth keeping:

- **Top block only.** The mid block stays the biome's own. `snow` is white on all six faces, so
  on its own this reads solid white on any slope; rock comes from the steep-rock pass.
- **"Capped" means snow was actually written.** Biomes that leave their top unset (Mesa, to keep
  its terracotta bands), quartz, and bare Tianzi cliffs never enter the stamp loop. The per-column
  capped flag is set inside it, so those columns are never treated as capped by the steep-rock or
  treeline passes. Setting it from the height test alone painted stone over terracotta.
- **Underwater tops are skipped**, using the same `topBlockUnderwater` as the grass rules rather
  than a sea-level test, because water level is per column.
- **The sub-block surface uses the fill loop's own threshold.** `terrainSurfaceValAt` is shared by
  the voxel fill and the snow line's surface height, including the per-voxel detail term. A copy
  of the formula would silently diverge the next time the threshold changes.

**Steep rock.** After the fill loop, capped columns whose surface gradient reaches
`snowSteepGradient` (1 = 45°) get rock instead of snow:

- The rock is the **landform's surface rock** (`SurfaceMaterials::Column::rock`: terracotta,
  Tianzi strata, red sandstone) or plain stone. Never the voxel the fill loop left at the top:
  that carries cave-biome rock theming, which the topsoil stamp always hides, and exposing it put
  large dark basalt patches across mountain faces.
- The gradient comes from the **sub-block surface height**, where terrain density crosses zero
  between the top block and the air above, not from `terrainTopY`. Whole-block heights quantize a
  central difference to multiples of 0.5, which lands exactly on a 45° threshold and turns rock
  into isolated speckles.
- It needs neighbor heights the stamp doesn't have yet, hence a separate pass. Neighbor chunks
  generate concurrently, so border columns use a one-sided difference; on the smooth sub-block
  surface that differs from the central one only by curvature, so no seam shows.

The cap doubles as the **treeline** for both placement paths: grid candidates are rejected in
capped columns, and exposed-surface placement (Tianzi's pines and shrubs) skips a capped column's
top voxel. Both read the capped flag rather than the ground block, since steep capped columns end
up as rock, which is valid ground for those gens. Shelves below a capped top and the snowy-grass
band stay plantable, which is where a real treeline sits. Decorators need nothing: every surface
decorator entry is restricted to supports like `GRASS_BLOCK`, which snow, snowy grass and rock
already fail.

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
