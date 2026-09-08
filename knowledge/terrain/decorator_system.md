_Last edited: 2026-09-07_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random single-block vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. At each air-above-solid transition in a column, one entry is sampled. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density (e.g. Plains has weight-15 AIR vs weight-14 total vegetation).

`groundBlocks` filtering lets entries restrict to specific surfaces (flowers only on grass, tiny cactus only on sand) without needing separate decorators.

## Cave floor decorator

The pass visits every air-above-solid transition in a column. A transition whose
ground sits below the column's terrain top (`Chunk::terrainTopY`, the highest
solid terrain block before structures) is underground — a cave floor or the
underside of an overhang — and uses `CaveBiomes::getCaveFloorDecorator()`;
the rest use the surface biome's decorator as before. Tree canopies don't confuse
this: leaves are structure blocks placed above the terrain top, so the grass under
a tree is still classed as surface. There is one cave decorator, not one per cave
biome, because no per-voxel cave biome is stored; an entry meant for one biome
scopes itself with ground blocks only that biome's skin produces (ferns on moss and
overgrown rock), while biome-agnostic entries (glowshrooms) set no filter. It draws
from its own per-chunk RNG stream so adding cave flora never shifts the surface
decorator pattern.

## Ordering Guarantees

Decorators run **after** structures in `fillStructuresAndDecorators`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The decorator RNG is seeded once per chunk (not per column). This means the same chunk always gets the same pattern, which matters for deterministic world generation, but adjacent columns within a chunk are correlated in their random draws.
