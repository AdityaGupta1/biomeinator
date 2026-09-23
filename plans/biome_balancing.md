# Biome Size Balancing

Goal: controllable biome distribution. Ordinary biomes should cover roughly equal shares of
land, and no biome should routinely appear as tiny slivers. Written 2026-09-22, assuming the
planned terrain regime table (a priority-ordered list of `{ biome, suitability, threshold,
fullStrength }` rows evaluated before the nearest-climate search) is in place.

## Why distribution is uneven today

- **Noise values are not uniform.** Fractal simplex output clusters around 0, so a biome whose
  climate point sits near the middle of climate space covers far more of the world than one
  near the edge, even if their Voronoi cells have equal volume in climate space.
- **Overrides cut into cells.** A regime claims part of an ordinary biome's cell and leaves a
  remnant (e.g. the dry override currently removes much of `DESERT`'s lowland cell).
- **Fast climate gradients.** A biome crossed quickly by the fields shows up as a thin band even
  when its total share is reasonable. This is a shape problem, not an area problem, and area
  balancing alone does not fix it.

## Measure first

Add a BiomeScanner report over many seeds (it already fills biome grids from the shared
`biome_noise` code):

- area share per biome (of all land, and of land not claimed by regimes)
- connected-patch size distribution per biome; the count of patches below a size cutoff is
  the sliver metric

Every later step is judged against these numbers.

## Regimes: thresholds from target shares

A regime claims a column when `suitability > threshold`; raising the threshold monotonically
shrinks its area. Instead of hand-tuning, sample the fields over a large land area and set each
threshold to the percentile matching its target share (6% of land → the 94th percentile of that
regime's suitability). `fullStrength` follows as a fixed offset or a further percentile.

Calibrate in priority order: each regime's percentile is taken over the samples earlier regimes
did not claim, matching the first-match lookup.

## Ordinary biomes: equalize axes, then relax

1. **Equalize each climate axis** by remapping it through its sampled CDF, so values are uniform
   on [0, 1]. After this, equal Voronoi volume ≈ equal world area. Only approximate if axes are
   correlated; check with the area report.
2. **Relax the climate points** so their cells have equal volume. Lloyd relaxation (centroidal
   Voronoi tessellation) is the standard deterministic form of "points repel until evenly
   spread", simpler than a physics simulation.
   - Anchor each point to its authored position with a spring or a bounding region, so biomes
     keep their meaning (tundra stays cold). Authoring sets *where* a biome lives; relaxation
     evens out *how much* it gets.
   - Relax over samples not claimed by regimes, so ordinary biomes share the remaining land
     evenly and regime cut-outs don't leave remnants.
   - Respect the existing candidate partitions (lowland/highland lists): relax each list over
     its own samples.

## Offline, baked constants

Run calibration offline (a scanner mode or a small tool) and bake the resulting thresholds, CDF
tables and climate points into constants, rather than computing at startup:

- Noise statistics are seed-independent (same FastNoise configuration), so results only change
  when biomes or noise configuration change.
- No startup cost, and no risk of floating-point differences between builds changing worlds.

Any change to a biome or target share re-balances everything jointly, shifting every biome's
placement. Acceptable before world compatibility matters; worth revisiting after.

## Shape (slivers not fixed by area)

Multi-axis suitability products (Tianzi's temperate × humid × preserved × inland window) tend
to produce stringy regions. Options, guided by the patch-size metric:

- smoother or larger-scale noise for the axes involved
- a minimum patch size baked into a regime's suitability (e.g. require suitability to stay high
  over a neighborhood, using a lower-frequency version of the field)

## Order

1. Terrain regime table (prerequisite: one threshold per regime).
2. Scanner area/patch report; record the current baseline.
3. Regime percentile calibration.
4. Axis equalization plus anchored relaxation for ordinary biomes.
5. Shape fixes where the patch metric still shows slivers.
