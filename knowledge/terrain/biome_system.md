_Last edited: 2026-04-26_

# Biome System

`src/terrain/biome.h/cpp` — distributes 11 biomes using a 4D noise-space selection system.

## Selection Logic

Biome assignment is NOT a simple nearest-neighbor in 4D space. The `inland` axis acts as a hard partitioner:

| Inland range | Category |
|---|---|
| < -0.15 | Ocean (1 biome) |
| -0.15 to 0.0 | Beach (3 biomes) |
| 0.0 to 0.85 | Lowland (4 biomes) |
| ≥ 0.85 | Highland (3 biomes) |

Within each category, selection is nearest-neighbor by squared distance in (temperature, humidity, peak) only — `inland` is not used for distance. This two-step approach means a hot beach can't accidentally become a desert even if their noise points are close in 4D.

## Per-Column Jitter

`BiomeNoise::randomOffset` adds tiny random offsets before selection. This softens biome boundaries — columns near an edge occasionally flip, creating a natural ragged border instead of a sharp line following an isosurface.

## BiomeData Role

Each biome's `BiomeData` bundles its noise target point, surface blocks, structure generators, and decorator. This is the single definition point for a biome's identity — adding a new biome means adding one entry to the enum and one initialization block.
