_Last edited: 2026-09-30_

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

## Integer range conversion

Integer ranges are half-open. Convert the nonnegative sampled offset to an integer before
adding the minimum: casting the shifted sample instead truncates negative values toward zero,
overweights zero in ranges crossing it, and can return the exclusive maximum for negative-only
ranges. The range must be nonempty and its width must fit in `int`.

Correcting this conversion on 2026-09-30 changes integer selections with negative minima,
including procedural noise offsets and formation choices. The hash, raw integer stream, float
stream, and number of draws are unchanged, but previously generated terrain for the same seed
can differ. Do not silently regenerate rendering reference images to hide that change.

The engine's HLSL RNG has no signed integer range helper. Its integer sampling consumers select
nonnegative indices, so their conversions do not have this negative-value truncation issue.
