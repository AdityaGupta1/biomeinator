_Last edited: 2026-09-22_

# Biome System

`src/terrain/biome.h/cpp` selects surface materials and vegetation from climate and terrain
suitability. The smooth fields also shape terrain independently of the selected biome; see
[terrain_profiles.md](terrain_profiles.md).

**Design rule: the terrain chooses the biome, not the other way around.** Elevation comes only
from the relief fields (peak, erosion, inland). A biome or regime may select surface blocks,
vegetation and bounded landform styles, and its label should derive from the same factors the
terrain uses (as `isHighland` does from `mountainPeakWeight`). It must never multiply elevation
by a climate or biome weight, or swap in a separate height profile: those produce steep pits and
walls wherever the weight changes faster than the relief. Tianzi's relief swap is the one
accepted exception, because its towers replace the relief they remove.

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

`evaluateRegimes` derives two weights per regime from the same suitabilities in one pass, returned
with the natural terrain so consumers never recompute them:

- **Landform** weight: 0 at the label threshold, 1 at full strength. It drives geometry
  (terraces, towers, spires, quartz, Tianzi soil and pillar seals).
- **Coverage** weight: 1 across the whole label, fading out just outside it. It drives styles
  that should cover the labelled area uniformly: roughness, Mesa detail, and yes/no rock materials
  via `NaturalTerrain::isCoveredBy` (coverage at least 0.5).

Both are multiplied down to 0 approaching the label of every higher-priority regime, over that
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

Roughness uses coverage rather than the landform ramp because blending by the landform weight left
the outer band of Mesa and red desert labels, where their landforms are still weak, with full
mountain roughness, which carved ravine-like gashes into rugged dry ground. Elsewhere roughness
follows relief alone. Roughness is not purely texture: terrain below the base height is denser, so
a larger amplitude raises the effective ground slightly. It therefore must not follow raw climate.
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
