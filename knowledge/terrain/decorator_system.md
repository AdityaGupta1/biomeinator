_Last edited: 2026-08-23_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random single-block vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. At each air-above-solid transition in a column, one entry is sampled. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density (e.g. Plains has weight-15 AIR vs weight-14 total vegetation).

`groundBlocks` filtering lets entries restrict to specific surfaces (flowers only on grass, tiny cactus only on sand) without needing separate decorators.

An entry with a `topBlock` places a two-tall plant (cattail + tip). If the block above the base is not AIR (e.g. under low tree canopy), nothing is placed at all — a base without its tip would look broken.

## Ordering Guarantees

Decorators run **after** structures in `fillStructuresAndDecorators`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The decorator RNG is seeded once per chunk (not per column). This means the same chunk always gets the same pattern, which matters for deterministic world generation, but adjacent columns within a chunk are correlated in their random draws.
