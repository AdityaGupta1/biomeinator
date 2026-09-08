_Last edited: 2026-09-07_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random single-block vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. At each air-above-solid transition in a column, one entry is sampled. The ground must be a full cube: decorators never stand on other decorators or on X-shaped structure flora. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density (e.g. Plains has weight-15 AIR vs weight-14 total vegetation).

`groundBlocks` filtering lets entries restrict to specific surfaces (flowers only on grass, tiny cactus only on sand) without needing separate decorators.

## Cave floor decorator

The pass visits every air-above-solid transition in a column. A transition whose
ground sits below the column's terrain top (`Chunk::terrainTopY`, the highest
solid terrain block before structures) is underground; the rest use the surface
biome's decorator as before. Tree canopies don't confuse this: leaves are structure
blocks placed above the terrain top, so the grass under a tree is still classed as
surface.

A transition is first matched against `Chunk::caveFloors`, the floor solids
captured during the terrain scan together with their cave biome (a few entries per
column, grouped by `caveFloorOffsets`; the scan is ascending so a single cursor
walks them), and a match applies `CaveBiomeData::decorator` of that biome. This is
the one place a per-position cave biome survives generation — far cheaper than a
per-voxel store and exactly what floor decoration needs. Cave floors must be checked
*before* the terrain-top test: a column whose topmost pocket opens to the sky has
no terrain top above its floors (`terrainTopY` stays 0), so a top-first test would
hand those floors the surface decorator. Ground that matches no floor and sits at
or above the terrain top is surface; other underground ground (overhang undersides,
blocks placed by structures) gets nothing. Cave draws come from their own per-chunk
RNG stream so adding cave flora never shifts the surface decorator pattern.

## Ordering Guarantees

Decorators run **after** structures in `runStructuresAndDecoratorPass`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The decorator RNG is seeded once per chunk (not per column). This means the same chunk always gets the same pattern, which matters for deterministic world generation, but adjacent columns within a chunk are correlated in their random draws.
