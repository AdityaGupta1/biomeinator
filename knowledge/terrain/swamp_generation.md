_Last edited: 2026-08-23_

# Swamp Generation

Swamps are cellular ponds: a sparse jittered grid of cell sites (`swampCellSize`), where each
swampy cell floods to its own flat water level derived from the local natural terrain. All
per-column shaping happens in `SwampShaping::computeShaping` (`swamp_shaping.cpp`).

## Ponds and dams

- A cell is swampy when the flood factor at its *site* exceeds `floodCellThreshold`. The biome is
  painted per column against the looser `floodTintThreshold`, so flooded terrain and the swamp
  biome overlap heavily but are deliberately not 1:1 — a pond near the swamp region's edge can
  keep another biome.
- The pond level tracks the second-lowest of nine natural-height samples across the cell, so a
  cell on sloped terrain floods its low side and a single deep corner can't drag the level below
  the rest of the cell and leave it dry. Boundary samples are shared with adjacent cells, which
  also nudges neighboring levels toward merging.
- A raised dam band along every cell border contains the water — two different water levels never
  touch, and the dam is the *only* containment mechanism, so no per-column gate may fully
  suppress it (this is why the containment band overrides the far-cell gate).
- Cells whose neighbors share a level merge into larger marshes by skipping the dam between them.

## Height shaping

Shaping is height-domain: natural terrain up to `swampPullDownStart` above the pond level is
pulled down to the marsh flat, then blends back to fully natural over `swampPullDownBlendRange`.
The transition width therefore scales with the natural slope, and tall terrain is never fought —
hills form the shore instead of a dam wall. The band is deliberately NOT widened by the flood
factor: a spatially varying band start turns flood-factor contours on hillsides into sunken dry
shelves (trenches); deep flood expresses itself through the marsh floor depth instead.

The nearest cell site decides a column's pond; every other scanned site contributes a dam barrier
profile, and the column takes the max over all of them (instead of just the second-nearest) so
the terrain stays continuous where the second-nearest site's identity flips, e.g. at three-cell
corners between merged and dry cells.

Dam contributions interpolate from an anchor blended across near-tied cells' shape heights, so
the anchor stays continuous when the nearest-cell identity flips at a swampy/dry border.
Anchoring at the own cell's shape height alone jumps there (marsh floor vs natural), cutting
walls wherever a farther cell's partial dam wins the max on only one side of the border.

The dam's rise above natural terrain fades via a proportional backside tail (the levee backside
slopes down instead of cliffing) and a constant-slope bank: full dam within the warp-jitter
radius so adjacent columns can never step from open water straight to an unraised column, then a
fixed run of blocks per block of rise, so tall banks fade over a proportionally longer distance
instead of becoming steeper.

The surface-noise multiplier flattens toward `swampTerrainSurfaceMultiplier` on a ramp keyed to
edge distance alone; gating it on the dam rise instead would flip the noise amplitude
discontinuously along the contour where natural height crosses dam height, cutting vertical
cliff faces there.

## Window stability

Column positions are domain-warped before the cell lookup so shorelines and dam bands wobble
organically instead of following straight Voronoi edges; the amplitude must stay well under
`swampCellPadding` so a warped column still finds its true nearest sites.

Each column scans a 5x5 cell window centered on its (warped) cell. Adjacent columns' windows can
be centered one cell apart, so any contribution from cells near the window edge must fade out
well inside the window or it seams along the window-flip contour (the same reason the scan is 5x5
rather than 3x3: a 3x3 can miss the true nearest site for one of two adjacent columns). Every
distance-dependent contribution — dam profile, multiplier flatten, cave seal — is therefore
scaled by a far-cell gate that reaches zero inside the guaranteed-shared radius. The near-edge
containment band overrides the gate because a genuine dam's cell site can legitimately sit beyond
the gate distance, and the gate scales the dam's whole profile toward the anchor so a gated-out
cell contributes nothing, not even its natural-height floor.

## Cave sealing

Bank columns have high base heights, so the base-relative cave fade alone would leave caves open
below a neighboring pond's waterline and expose its water sideways. Each column therefore lowers
the cave carving threshold at and below the max gate-scaled pond level of the swampy cells in its
scan window.
