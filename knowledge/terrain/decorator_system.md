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

The surface-biome column pass skips empty decorators and starts immediately above
`terrainTopY`, with the terrain-top block as its initial support. Lower cells cannot
place a surface decorator or consume its random stream. It still scans the rest of
the column above that point so structures can provide higher supports. Cave-marked
cells remain excluded, and tree canopies do not change the terrain-top classification.

The cave word filter retains all six directional support masks for its per-face
tests, avoiding another world-to-chunk lookup for each face. A face normal points
from the support toward the candidate, so the support lies in the opposite
direction. Vertical masks include carries from adjacent words, including terrain
above the cave-height cap. Face enumeration order remains unchanged because it
determines the position-hashed face choice.

## Ordering Guarantees

Decorators run **after** structures in `runStructuresAndDecoratorPass`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

## RNG Is Per-Chunk

The surface decorator RNG remains seeded once per chunk. Cave placement is hashed
from world position so adding a new candidate elsewhere does not perturb it.
