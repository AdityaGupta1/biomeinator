_Last edited: 2026-04-27_

# Chunk Generator

`src/terrain/chunk_generator.h/cpp` — initializes FastNoise2 noise graphs. The actual generation runs in `Chunk::fillTerrainBlocksAndCreateStructures()`.

## Noise Architecture

Six noise graphs, all using FastNoise2's node-graph API. Four are 2D (biome axes: temperature, humidity, peak, inland) and two are 3D (terrain surface, caves). A random `noiseOffsetXZ` derived from the world seed shifts all sample positions so different seeds produce different terrain even though node seed offsets are hardcoded.

## Cave Noise Design

Two cave noise sources: **worley** (cellular, produces rounded tunnels) below `0.4 * terrainBaseHeight`, **simplex** (spaghetti-style) above `0.6 * terrainBaseHeight`. Between those bounds, blends through `min(worley, simplex)` at the midpoint — this avoids abrupt transitions and lets the more open of the two dominate in the overlap zone.

Two mechanisms suppress caves near the surface:
- **Surface fade**: `caveSurfaceVal` ramps down approaching `terrainBaseHeight`, making the threshold harder to meet and closing caves near the terrain surface.
- **Altitude squash**: above y=256 an additive term on `caveSurfaceVal` smoothly closes caves so tall mountain peaks remain solid.

## Heightfield Design

The terrain isn't a simple heightmap — it uses a 3D surface threshold (`terrainNoise < surfaceVal`) so overhangs can form. But the threshold is shaped by a per-column `terrainBaseHeight` and `terrainSurfaceMultiplier`:

- **Below base height**: the surface multiplier is doubled (`terrainBelowHeightfieldSurfaceMultiplier = 2`), which makes underground much more uniformly solid and flattens the base. Without this, you'd get as many air pockets below as above.
- **Near coast** (`inland` near 0): base height is pulled toward `seaLevel + 8` via smoothstep, creating gentle shorelines rather than cliffs.
- **Mountains**: `peak^4 * inland` adds up to ~135 blocks of additional height, but only when both peak ridgeline and inland values are high.

## 3D Noise Bounds Optimization

The 3D terrain noise is only sampled in the Y range that could possibly contain the surface (derived from `surfaceValBound / multiplier`). For flat biomes this might be a 30-block band; for mountains it's larger. This avoids sampling noise for blocks that are trivially underground or trivially air.

## Structure Creation Happens Here

After blocks are filled, structure candidates are generated using the heightfield (which is in scratch memory and would be lost after this task) and biome data. See [structure_system.md](structure_system.md) for the placement algorithm.
