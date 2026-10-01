_Last edited: 2026-09-30_

# Decorator System

`src/terrain/structure/decorator.h/cpp` — weighted random vegetation placement on terrain surfaces.

## Design

Each biome has a `Decorator` — a weighted list of blocks. Surface-biome decorators are sampled at air-above-solid transitions. Cave decorators are sampled once per cave-air voxel bordering an eligible full-cube terrain support. AIR entries in the weight pool act as "nothing placed" outcomes, controlling density.

A block with an `upperHalf` (see [block_system.md](block_system.md)) also fills the cell above.
If that cell isn't air, nothing is placed, but the draw has already consumed its RNG, so a
blocked two-tall plant doesn't shift later placements. Both passes go through
`Chunk::tryPlaceDecorator`, so cave floors can use two-tall plants too.

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

## Drifts

Drift entries pool their weight like ordinary entries, but the species drawn comes from a
per-patch value (`driftSample` in `chunk.cpp`, about 20-block cells with warped borders) rather
than the column's RNG. A patch of meadow therefore grows only one flower, which reads much calmer
than an even mix of every species. Density stays per-column, so only the species choice is
clustered. Drift members must share support blocks, since the support test runs on the rolled
entry before the species is swapped.

## Ordering Guarantees

Decorators run **after** structures in `runStructuresAndDecoratorPass`. Since decorators only write into AIR blocks, tree trunks/leaves placed by structures are never overwritten. Conversely, decorators can place blocks at the base of trees where air still exists.

Snow layers are placed between the two (see [chunk_generator.md](chunk_generator.md#snow-layers)), so a covered cell is no longer air and gets no decorator. That also shifts which cells consume the per-chunk surface RNG, so adding or tuning layers reshuffles decorators in affected chunks.

## RNG Is Per-Chunk

The surface decorator RNG remains seeded once per chunk. Cave placement is hashed
from world position so adding a new candidate elsewhere does not perturb it.
