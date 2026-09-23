_Last edited: 2026-09-22_

# Biome System

`src/terrain/biome.h/cpp` selects surface materials and vegetation from climate and terrain
suitability. The smooth fields also shape terrain independently of the selected biome; see
[terrain_profiles.md](terrain_profiles.md).

This entry covers **surface** biomes. Underground stone is themed separately by
the 3D [cave_biome_system.md](cave_biome_system.md).

## Selection Logic

Biomes whose label must agree with a landform are **terrain regimes** (swamp, Tianzi, Mesa,
red desert), defined in one priority-ordered table in `biome_noise.cpp`. Each regime combines
its axes into a single suitability; `biomeFromNoise` returns the first regime whose suitability
exceeds its threshold. Everything else uses the nearest climate/peak target: ocean and beach are
partitioned by inlandness first, keeping climate from turning coastlines into inland biomes, then
lowland or highland candidates. Highland means far inland (as originally) or strong
`highlandReliefWeight`, the same preserved-relief factor that raises mountain terrain. Sharing it
keeps low-erosion highland labels from reaching the shore ahead of the relief, which painted
mountain stone directly behind beaches.
Regime suitabilities are zero on the coast, so checking them before that partition is safe.

A nearest-target search cannot express these regimes: Tianzi is a window across temperature,
humidity, preserved relief and inlandness, and a Voronoi cell gives no terrain strength that
vanishes at its border. Add a new landform biome as a regime row, not as another special case
in the selection code.

`regimeWeight` derives the terrain strength from the same suitability: 0 at the label threshold,
1 at `fullStrength`. It is also multiplied down to 0 approaching the label of every
higher-priority regime, over that regime's ramp width mirrored below its threshold. So a
landform never extends past its label, and overlapping regimes (red desert spires under Mesa)
need no special-case masks. Apply the ramp **after** combining every suitability axis:
separately fading inlandness let coastal columns keep tall pillars after the label had already
switched to tundra. Shape, formation rock and cliff soil use the unjittered strength; biome
jitter can affect the negligible outer foothills but cannot cut through a tower's core.

Swamp's row sets its label and how other regimes fade next to wetlands; its terrain comes from
flood cells with their own threshold (see [swamp_generation.md](swamp_generation.md)).

Erosion controls suitability, not nearest-target distance. Its thresholds belong to shared
terrain regimes rather than individual biome height overrides. The swamp flood factor also
requires eroded terrain. Oases are spatial pond footprints and override the label in both the
chunk generator and map, after climate selection; a climate-only lookup cannot locate one.
See [swamp_generation.md](swamp_generation.md) for the other local-water override.

## Per-Column Jitter

`BiomeNoise::randomOffset` adds tiny random offsets before selection. This softens biome boundaries — columns near an edge occasionally flip, creating a natural ragged border instead of a sharp line following an isosurface.

## BiomeData Role

Each biome's `BiomeData` bundles its noise target point, surface blocks, structure generators, and decorator. This is the single definition point for a biome's identity — adding a new biome means adding one entry to the enum and one initialization block.

Landform-specific rock is applied separately from topsoil. Mesa deliberately leaves its top
and mid blocks unset, preserving elevation-based terracotta bands instead of repainting every
column with the same cap. Append biome/structure enum values: world exports serialize them.
