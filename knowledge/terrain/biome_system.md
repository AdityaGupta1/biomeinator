_Last edited: 2026-09-21_

# Biome System

`src/terrain/biome.h/cpp` selects surface materials and vegetation from climate and terrain
suitability. The smooth fields also shape terrain independently of the selected biome; see
[terrain_profiles.md](terrain_profiles.md).

This entry covers **surface** biomes. Underground stone is themed separately by
the 3D [cave_biome_system.md](cave_biome_system.md).

## Selection Logic

Ocean and beach are partitioned by inlandness first, keeping climate from turning coastlines
into inland biomes. Tianzi suitability combines humidity, a temperate-to-warm temperature
window, preserved relief and inlandness. Its label and terrain strength derive from the same
combined value; checking erosion first used to claim cold/dry mountain regions as Tianzi.
Hot, dry terrain becomes Mesa where terraces dominate, Red Desert where ruggedness
dominates, and ordinary desert in the eroded regime. Remaining terrain uses the original
nearest climate/peak target among lowland or highland candidates; low erosion can enable
highlands without requiring extreme inlandness.

The strength ramp starts at the biome threshold and reaches full pillars farther inside.
Apply it **after** combining every suitability axis: separately fading inlandness let coastal
columns keep tall pillars after the label had already switched to tundra. Shape, formation
rock and cliff soil use the unjittered strength; biome jitter can affect the negligible outer
foothills but cannot cut through a tower's core.

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
