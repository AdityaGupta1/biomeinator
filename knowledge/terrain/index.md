_Last edited: 2026-04-26_

# Terrain Knowledgebase

Procedural voxel world: chunk lifecycle, noise generation, biomes, structures, and mesh building.

| Entry | Description |
|---|---|
| [terrain_manager.md](terrain_manager.md) | Top-level Terrain class, render distance, chunk creation/destruction |
| [region_system.md](region_system.md) | Region: 32×32 chunk spatial grouping, neighbor lookups |
| [chunk_state_machine.md](chunk_state_machine.md) | Multi-stage ChunkState, transitions, parallelism constraints |
| [chunk_segments.md](chunk_segments.md) | 4×8×4 ChunkSegment subdivision, AIR/SOLID_SURROUNDED/MIXED culling |
| [chunk_generator.md](chunk_generator.md) | FastNoise2-based height maps, cave generation, biome allocation |
| [biome_system.md](biome_system.md) | Voronoi biome distribution, BiomeNoise parameters, all 11 biomes |
| [block_system.md](block_system.md) | Block enum, BlockData (textures, type, shape), emissive blocks |
| [structure_system.md](structure_system.md) | StructureGen grid placement, structure types, StructureBounds |
| [decorator_system.md](decorator_system.md) | Per-biome vegetation decorators, weighted random block placement |
| [greedy_meshing.md](greedy_meshing.md) | Voxel-to-mesh greedy merge, UV assignment, crack prevention |
