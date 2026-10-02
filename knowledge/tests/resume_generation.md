_Last edited: 2026-09-30_

# Resuming generation from an imported world

Recorded as a reference diff rather than live code:
[resume_generation_reference.patch](resume_generation_reference.patch). It is a disposable CPU
harness that checks whether chunks generated fresh beside imported ones end up identical to an
uninterrupted run. Neither the unit tests nor the rendering tests can answer that. The unit
target cannot link the generator, and rendering tests load fully baked worlds without generating
across their edge (see [world export/import](../terrain/world_export_import.md)). It is too heavy
to keep as a permanent target, but worth rebuilding whenever saved generation inputs change.

**Read it, don't replay it.** It is a snapshot of the September 2026 tree (region format v7).
Its source list, renderer stubs, and the Chunk and RegionFile calls will drift. Reimplement it
against the current tree under an ignored `build/<experiment>/` directory, following the build
approach in [CPU terrain experiments](cpu_terrain_benchmarks.md): real generator sources,
minimal renderer substitutes, and the main build's prebuilt FastNoise and lz4 libraries.

## What it checks

For each test location it generates a 7x7 chunk area, finishes the western half, and exports
it. It then imports that half into a new world, generates the rest fresh, and compares final
blocks and block states with the uninterrupted world.

- **Seam chunks** are the fresh column touching the imported chunks. Their structure
  neighborhood includes imported data, so any lost or reordered generation input shows up
  here.
- **Control chunks** are one column further out, whose neighborhoods are entirely fresh. They
  must always match. A mismatch there means the harness itself diverges, not the format.
- The restored world finishes its fresh chunks in the opposite order. Structure filling is
  order-independent, so this also catches order dependence across the seam.

Exports go through real region files rather than copying `SerializedChunkData`, so the codec
and the generation inputs are tested together.

## Choosing locations

Partial snow cover is where the snow slope and hollow tests decide the outcome, and those
tests sample neighboring chunks' terrain heights. The harness screens terrain-only chunks for
partially snowy columns and adds evenly spread non-snow locations. The non-snow locations
cover cave decorations, cave structures and exposed-surface trees. Use several seeds. A
single seed or the origin alone rarely puts snow, trees and caves on the seam together.

## Proving the check can fail

A passing run means little unless the harness also detects missing inputs. The reference
builds a second variant from the previous format's sources, extracted with
`git archive <rev> src`. That variant imports chunks the way its exporter did: final blocks,
biomes, accepted structures and block states only. It shares generated headers and libraries
with the current build. Against v6-style data it failed 25 of 56 locations: snow layers,
pine trees and cave decorations differed at the seam, while every control chunk still matched.
When changing the format, keep a variant that omits the input you are adding, and check that
the harness catches the omission.

## Gotchas

- **Region file grouping.** Exported chunks may span several regions. Group them by
  `floorDiv(chunk, regionSideLength)` before writing each region.
- **Load imported chunks before wiring and generation.** `loadSerializedData` requires a chunk
  still in `NEEDS_TERRAIN`.
- **Reinitialize `ChunkGenerator` after changing the seed**, and override the seed through
  the stubbed `SettingsManager::getWorldSeed`.
- **Generated block IDs come from the main build.** That is valid only while the variants
  share assets. Regenerate them if the compared revisions change block definitions.
