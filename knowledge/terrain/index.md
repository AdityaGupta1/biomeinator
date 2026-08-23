_Last edited: 2026-08-23_

# Terrain Knowledgebase

Procedural voxel world: chunk lifecycle, noise generation, biomes, structures, and mesh building.

| Entry | Description |
|---|---|
| [terrain_manager.md](terrain_manager.md) | Top-level Terrain class, render distance, chunk creation/destruction |
| [region_system.md](region_system.md) | Region: 32×32 chunk spatial grouping, neighbor lookups |
| [chunk_state_machine.md](chunk_state_machine.md) | Multi-stage ChunkState, transitions, parallelism constraints |
| [chunk_segments.md](chunk_segments.md) | 4×8×4 ChunkSegment subdivision, AIR/SOLID_SURROUNDED/MIXED culling |
| [chunk_generator.md](chunk_generator.md) | FastNoise2-based height maps, cave generation, biome allocation |
| [biome_system.md](biome_system.md) | Voronoi biome distribution, BiomeNoise parameters, swamp override |
| [cave_biome_system.md](cave_biome_system.md) | 3D cave biome noise, downsampled classification, surface bias, stone theming |
| [block_system.md](block_system.md) | Block enum, BlockData (textures, type, shape), emissive blocks |
| [structure_system.md](structure_system.md) | StructureGen grid placement, structure types, StructureBounds |
| [cave_structure_system.md](cave_structure_system.md) | Underground floor/ceiling structures, column-centric placement, CaveLayer capture, layer-index seed |
| [decorator_system.md](decorator_system.md) | Per-biome vegetation decorators, weighted random block placement |
| [greedy_meshing.md](greedy_meshing.md) | Voxel-to-mesh greedy merge, UV assignment, crack prevention |
| [terrain_omm.md](terrain_omm.md) | Opacity micromap baking for cutout tiles, exactness argument, build ordering |
| [world_export_import.md](world_export_import.md) | Serialize/restore terrain to disk; early-return invariant, import-side gotchas |
| [swamp_generation.md](swamp_generation.md) | Cellular pond/dam design, window stability, cave sealing |
