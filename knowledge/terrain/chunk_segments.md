_Last edited: 2026-04-26_

# Chunk Segments

Each chunk is subdivided into 4×8×4-block segments (1024 per chunk). Each segment is classified as AIR, SOLID_SURROUNDED, or MIXED. Only MIXED segments are iterated during mesh generation — this skips the vast underground stone and upper air.

## SOLID_SURROUNDED Logic

A fully-solid segment can still have visible faces if its boundary is exposed. `SOLID_SURROUNDED` means all 6 bounding faces (one block thick) are also solid — no face in this segment could ever be visible.

The non-obvious optimization: when checking a segment's -x/-y/-z neighbor face, if the adjacent segment was already classified `SOLID_SURROUNDED`, the check is skipped (that face is guaranteed solid). This cascading saves redundant block iteration deep underground.

**Top/bottom Y segments** are never `SOLID_SURROUNDED` — they border the world boundary where no blocks exist, so they always have potentially-visible faces.

## Why Segments Need Neighbors

Classification requires reading one-block-thick strips from adjacent chunks (for edge segments). This is why `generateSegments` waits until all 4 cardinal neighbors have their blocks (`NEEDS_SEGMENTS` depends on `numNeighborsWithBlocks == 4`).

## Memory

The `prevSegments` working array is allocated from `ThreadMemoryAllocator` (stack-style scratch memory) and discarded after classification. Only `segmentsToGenerate` (the MIXED segment positions) persists on the chunk.
