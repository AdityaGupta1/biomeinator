_Last edited: 2026-09-20_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random single-block vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. Surface-biome decorators are sampled at air-above-solid transitions. Cave decorators are sampled once per cave-air voxel bordering an eligible full-cube terrain support. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density.

Support-block filtering lets entries restrict placement to particular blocks. A surface mask independently permits floors, walls, or ceilings; it defaults to floors so ordinary vegetation remains upright. Eligible surface/support pairs are indexed when entries are registered, so probing six neighboring faces does not repeatedly scan the weighted pool. If several eligible faces border one cave-air voxel, a position hash chooses one before the weighted draw, preventing corner density from multiplying.

## Cave surface decorator

Terrain generation retains a bit mask of original cave air and the coarse temperature/
humidity fields with their column biases. The decorator pass runs after structures and
requires the target still to be AIR. Before classifying its biome, it intersects the
cave-air mask with cells adjacent to immutable terrain full cubes, using word shifts for
vertical faces and the neighboring column masks for horizontal faces. This excludes cave
interiors cheaply and works across chunk boundaries. The ordinary per-face tests still
validate the biome's support rules. Cave placement decisions use separate
position-hashed streams for face choice and weighted sampling, so traversal changes
do not shift unrelated results. Candidates retain the original column and bottom-up
order. The retained cave fields, biases, and cave-air mask are released immediately after
this pass; neighboring chunks never read them.

The surface-biome column pass skips cave-marked cells and otherwise still uses
`terrainTopY`. Tree canopies therefore do not confuse surface classification.

## Ordering Guarantees

Decorators run **after** structures in `runStructuresAndDecoratorPass`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The surface decorator RNG remains seeded once per chunk. Cave placement is hashed
from world position so adding a new candidate elsewhere does not perturb it.
