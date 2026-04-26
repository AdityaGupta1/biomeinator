_Last edited: 2026-04-26_

# RNG

`src/util/rng.h` — PCG-style hash-based random number generator used for all procedural generation.

## Design Choice: Hash-Based, Not LCG

Each `nextUint()` call hashes the current seed to produce both the output and the next state. This means you can "fork" independent RNG streams by seeding from different inputs without correlation artifacts — critical for deterministic chunk generation where each chunk/structure/decorator needs its own independent stream seeded by position.

## `initRng` Overloads

The multi-argument `initRng(seed1, seed2, ...)` functions fold multiple values into a single seed via chained hashing. This is how position-dependent RNG streams are created: `initRng(worldSeed ^ constant, x, z)` gives a unique but deterministic stream for each column/chunk/structure.

## Determinism Contract

Same seed → same sequence, always. The terrain system relies on this: a structure's shape is determined by its world position, so any chunk can fill that structure's blocks and get the same result. Breaking this contract (e.g. by adding state) would cause cross-chunk structure inconsistencies.

## `nextFloat` Range

Returns [0, 1) — masks the bottom 24 bits (`& 0x00FFFFFF`) and divides by 2^24. Not full float precision but sufficient for procedural generation.
