_Last edited: 2026-09-09_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random single-block vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. Surface-biome decorators are sampled at air-above-solid transitions. Cave decorators are sampled once per cave-air voxel bordering an eligible full-cube terrain support. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density.

Support-block filtering lets entries restrict placement to particular blocks. A surface mask independently permits floors, walls, or ceilings; it defaults to floors so ordinary vegetation remains upright. If several eligible faces border one cave-air voxel, a position hash chooses one before the weighted draw, preventing corner density from multiplying.

## Cave surface decorator

Terrain generation retains one byte per voxel identifying the biome of carved cave
air; `0xff` means non-cave. The decorator pass runs after structures, requires the
target still to be AIR, and uses the immutable terrain-air mask to reject supports
that were introduced by structures. This exact post-structure neighbor test works
across chunk boundaries and avoids wall seams. Cave placement decisions use separate
position-hashed streams for face choice and weighted sampling, so traversal changes
do not shift unrelated results.

The surface-biome column pass skips cave-marked cells and otherwise still uses
`terrainTopY`. Tree canopies therefore do not confuse surface classification.

## Ordering Guarantees

Decorators run **after** structures in `runStructuresAndDecoratorPass`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The surface decorator RNG remains seeded once per chunk. Cave placement is hashed
from world position so adding a new candidate elsewhere does not perturb it.
