_Last edited: 2026-09-22_

# Biome System

`src/terrain/biome.h/cpp` selects surface materials and vegetation from climate and terrain
suitability. The smooth fields also shape terrain independently of the selected biome; see
[terrain_profiles.md](terrain_profiles.md).

**Design rule: the noise chooses both the biome and the terrain, separately.** Biome labels and
terrain parameters (elevation, roughness, fine detail, landforms) are independent functions of
the same noise fields. They line up because they read the same fields, not because one reads
the other: red desert is smooth because the dry, rugged noise that makes it red desert also
drives the smoothing, never because terrain reads the label. Vanilla Minecraft works the same
way, with offset/factor/jaggedness splines over continentalness, erosion and peaks alongside a
separate biome lookup.

Two consequences:
- Elevation comes only from the relief fields (peak, erosion, inland). Nothing multiplies
  elevation by a climate or biome weight, or swaps in a separate height profile. Tianzi's relief
  swap is the one accepted exception, because its towers replace the relief they remove.
- Every terrain-parameter ramp is sized for spatial smoothness, not just smoothness on its field
  axis. Labels are sharp by design; a parameter that switches at a label edge inherits that
  sharpness. Fields can change quickly in blocks (the dryness ramp spans ~16 blocks in places),
  so a ramp that looks gentle in field units can still be a cliff on the ground. Measure it: sweep
  `computeNaturalTerrain` over several seeds and count large changes between nearby samples.

Labels should derive from the same factors terrain uses (as `isHighland` does from
`mountainPeakWeight`), and yes/no materials may follow labels: materials are biome identity.

This entry covers **surface** biomes. Underground stone is themed separately by
the 3D [cave_biome_system.md](cave_biome_system.md).

## Selection Logic

Biomes whose label must agree with a landform are **terrain regimes** (swamp, Tianzi, Mesa,
red desert), defined in one priority-ordered table in `biome_noise.cpp`. Each regime combines
its axes into a single suitability; `biomeFromNoise` returns the first regime whose suitability
exceeds its threshold. Regime suitabilities are zero on the coast, so checking them first is safe.

Every other biome declares a **tier** in its `BiomeData`, and relief plus coastline pick the tier:
ocean and beach by fixed inland cutoffs, then highland where `isHighland` holds, else lowland.
`isHighland` thresholds `mountainPeakWeight`, the same term that raises tall peaks, so highland
labels sit exactly where mountain relief does: not on low-peak rugged ground (stone "mountains"
tops on flat land) and not ahead of the relief at coasts (mountain stone directly behind
beaches). Highland holds relief biomes only; climate zones without relief of their own (savanna,
ice fields) are lowland.

Within a tier, the nearest **climate target** (temperature, humidity) wins. Relief takes no part:
it already chose the tier, and matching on it again split neighboring targets along relief
contours and sent high-peak lowland to whichever low-peak target was least wrong (forest on hot,
dry high ground).

A nearest-target search cannot express regimes: Tianzi is a window across temperature,
humidity, preserved relief and inlandness, and a Voronoi cell gives no terrain strength that
vanishes at its border. Add a new landform biome as a regime row, not as another special case
in the selection code.

## Regime weights

`evaluateRegimes` derives three weights per regime in one pass, returned with the natural terrain
so consumers never recompute them:

- **Landform** weight: 0 at the label threshold, 1 at full strength. It drives geometry
  (terraces, towers, spires, quartz, Tianzi soil and pillar seals), which only changes height
  by bounded amounts.
- **Coverage** weight: 1 across the whole label, fading out just outside it. It drives yes/no
  rock materials via `NaturalTerrain::isCoveredBy` (coverage at least 0.5).
- **Style** weight: the same suitability evaluated through **widened** ramps (every factor's
  smoothstep stretched about its center by the row's `styleWiden`), ramped over the fade band
  below the threshold. It is full by the label edge and fades out well beyond it. Continuous
  styles (roughness, Mesa fine detail) use it, so they never change abruptly at a label edge.

Landform and coverage weights are multiplied down to 0 approaching the label of every higher-priority regime, over that
regime's fade width just below its threshold. So a regime's landform never extends past its label,
and overlapping regimes (red desert spires under Mesa) need no special-case masks. Apply the ramp
**after** combining every suitability axis: separately fading inlandness let coastal columns keep
tall pillars after the label had already switched to tundra. Everything uses the unjittered
weights; biome jitter can affect the negligible outer foothills but cannot cut through a tower.

Rows store the strength range and fade width **relative to the threshold**, so recalibrating a
threshold (e.g. from a target area share, see `plans/biome_balancing.md`) keeps them valid. The
fade fraction must stay below 1, checked at compile time: suitabilities bottom out at 0, so a fade
reaching below 0 suppresses lower regimes everywhere (this once capped red desert spires at 31%
height far from any Mesa).

Roughness (density amplitude) follows relief, blended toward each regime's roughness by its style
weight. It sets how far 3D noise pushes the surface, so a roughness contrast over a few blocks
stands up as a wall. Two earlier versions failed this way:
- blending by the landform weight left the outer band of Mesa and red desert labels with full
  mountain roughness: ravine-like gashes into rugged dry ground;
- blending by coverage fixed that but switched roughness within 4–8 blocks at the label edge:
  walls of 40+ blocks where red desert (roughness 12) met rugged mountains (~60). Red desert is
  worst because its suitability requires rugged ground, where relief roughness is highest.
`styleWiden` per regime sets how soft each edge is: red desert softest (7.5), Mesa a little
crisper (4.5), Tianzi crisper still (2, its four-factor product is already soft). All are several
times softer than the label ramps; a sweep then finds no roughness change above 16 per 4 blocks at
any regime edge. The widths depend on how fast the fields change in blocks, so re-measure after
changing a noise scale.

Yes/no materials use a 0.5 cutoff because coverage starts well below the label threshold; testing
`> 0` spread terracotta over an extra area about a fifth the size of Mesa itself.

Swamp's row sets its label and how other regimes fade next to wetlands. Its terrain comes from
flood cells with their own threshold (see [swamp_generation.md](swamp_generation.md)), but the
depth ramp ends at the same `floodFullStrength` as the row, so swamp is tuned in one place. The
flood factor's flatness term reads `ruggedWeight`, the same relief factor terrain uses.

Oases are spatial pond footprints and override the label in both the chunk generator and map,
after climate selection; a climate-only lookup cannot locate one. A pond only activates where no
regime claims any part of its footprint (`isClaimedByRegime` at the center and two rings): the
pond forces an absolute floor and rim, and scaling it down near a regime landform would breach its
water containment. Testing the label rather than coverage is enough, since landforms only exist
inside their labels. The footprint check makes oases about three times rarer than a center-only
check, because their dry, flat climate usually borders Mesa or red desert.

## Per-Column Jitter

`BiomeNoise::randomOffset` adds tiny random offsets before selection. This softens biome boundaries — columns near an edge occasionally flip, creating a natural ragged border instead of a sharp line following an isosurface.

## BiomeData Role

Each biome's `BiomeData` bundles its tier, climate target, surface blocks, structure generators, and decorator. This is the single definition point for a biome's identity — adding a new biome means adding one entry to the enum and one initialization block; the candidate lists are derived from the tiers.

Landform-specific rock is applied separately from topsoil. Mesa deliberately leaves its top
and mid blocks unset, preserving elevation-based terracotta bands instead of repainting every
column with the same cap. Append biome/structure enum values: world exports serialize them.
