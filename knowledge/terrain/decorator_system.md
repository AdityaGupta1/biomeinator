_Last edited: 2026-09-07_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random single-block vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. At each air-above-solid transition in a column, one entry is sampled. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density (e.g. Plains has weight-15 AIR vs weight-14 total vegetation).

`groundBlocks` filtering lets entries restrict to specific surfaces (flowers only on grass, tiny cactus only on sand) without needing separate decorators.

## Cave floor decorator

The pass visits every air-above-solid transition in a column, so cave floors are
already reached — but with the *surface* biome's decorator, whose ground filters
reject cave blocks. `CaveBiomes::getCaveFloorDecorator()` is a single decorator
used instead whenever the ground is a cave-flora block (moss, overgrown rock). It is
keyed by ground block, not cave biome, because no per-voxel cave biome is stored;
that works because those ground blocks are produced only by one biome's skin. It
draws from its own per-chunk RNG stream so adding cave flora never shifts the
surface decorator pattern.

## Ordering Guarantees

Decorators run **after** structures in `fillStructuresAndDecorators`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The decorator RNG is seeded once per chunk (not per column). This means the same chunk always gets the same pattern, which matters for deterministic world generation, but adjacent columns within a chunk are correlated in their random draws.
