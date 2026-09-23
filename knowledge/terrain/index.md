_Last edited: 2026-09-22_

# Terrain Knowledgebase

Procedural voxel world: chunk lifecycle, noise generation, biomes, structures, and mesh building.

| Entry | Description |
|---|---|
| [terrain_manager.md](terrain_manager.md) | Top-level Terrain class, render distance, chunk creation/destruction |
| [region_system.md](region_system.md) | Region: 32×32 chunk spatial grouping, neighbor lookups |
| [chunk_state_machine.md](chunk_state_machine.md) | Multi-stage ChunkState, transitions, parallelism constraints |
| [chunk_segments.md](chunk_segments.md) | 4×8×4 ChunkSegment subdivision, AIR/SOLID_SURROUNDED/MIXED culling |
| [chunk_generator.md](chunk_generator.md) | FastNoise2-based height maps, cave carving, coarse cave fields, biome allocation |
| [CPU terrain experiments](../tests/cpu_terrain_benchmarks.md) | Temporary benchmark harness to adapt for algorithm comparisons, with timing and correctness gotchas |
| [biome_system.md](biome_system.md) | Noise-chooses-biome-and-terrain rule, regime table and weights, tiers and climate targets |
| [terrain_profiles.md](terrain_profiles.md) | Shared erosion shaping, reusable formations, strata and contained pond oases |
| [cave_biome_system.md](cave_biome_system.md) | 3D cave biome noise, downsampled classification, surface bias, carve-noise skin/fringe, secondary rock |
| [block_system.md](block_system.md) | JSON block definitions, generated Block enum, BlockData, emissive blocks |
| [custom_models.md](custom_models.md) | Cached GLB decorator geometry, placement, rotation, opaque-atlas contract |
| [structure_system.md](structure_system.md) | Ground grids, reusable ledge placement, clearance/spacing and cross-chunk filling |
| [cave_structure_system.md](cave_structure_system.md) | Underground floor/ceiling structures, column-centric placement, CaveLayer capture, terrain air mask, type-major fill order |
| [decorator_system.md](decorator_system.md) | Per-biome vegetation decorators, weighted random block placement, all-face cave decorators |
| [greedy_meshing.md](greedy_meshing.md) | Voxel-to-mesh greedy merge, UV assignment, crack prevention |
| [terrain_omm.md](terrain_omm.md) | Opacity micromap baking for cutout tiles, exactness argument, build ordering |
| [world_export_import.md](world_export_import.md) | Serialize/restore terrain to disk; early-return invariant, import-side gotchas |
| [swamp_generation.md](swamp_generation.md) | Cellular pond/dam design, window stability, cave sealing |
| [biome_scanner.md](biome_scanner.md) | BiomeScanner web tool, biome_noise module extraction rationale |
