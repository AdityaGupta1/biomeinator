_Last edited: 2026-09-22_

# Biome System

`src/terrain/biome.h/cpp` selects surface materials and vegetation from climate and terrain
suitability. The smooth fields also shape terrain independently of the selected biome; see
[terrain_profiles.md](terrain_profiles.md).

**Design rule: the terrain chooses the biome, not the other way around.** Elevation comes only
from the relief fields (peak, erosion, inland). A biome or regime may select surface blocks,
vegetation and bounded landform styles, and its label should derive from the same factors the
terrain uses (as `highlandReliefWeight` does for highland candidates). It must never multiply
elevation by a climate or biome weight, or swap in a separate height profile: those produce
steep pits and walls wherever the weight changes faster than the relief. Tianzi's relief swap
is the one accepted exception, because its towers replace the relief they remove.

This entry covers **surface** biomes. Underground stone is themed separately by
the 3D [cave_biome_system.md](cave_biome_system.md).

## Selection Logic

Biomes whose label must agree with a landform are **terrain regimes** (swamp, Tianzi, Mesa,
red desert), defined in one priority-ordered table in `biome_noise.cpp`. Each regime combines
its axes into a single suitability; `biomeFromNoise` returns the first regime whose suitability
exceeds its threshold. Everything else uses the nearest climate/peak target: ocean and beach are
partitioned by inlandness first, keeping climate from turning coastlines into inland biomes, then
lowland or highland candidates. Highland means strong `highlandReliefWeight`, the same
preserved-relief factor that raises mountain terrain, and nothing else. Sharing it keeps highland
labels from reaching the shore ahead of the relief, which painted mountain stone directly behind
beaches. Highland candidates are relief biomes; climate zones without relief of their own
(savanna, ice fields) are lowland candidates. A former "far inland means highland" rule had no
terrain counterpart and put highland labels on flat, eroded interior.
Regime suitabilities are zero on the coast, so checking them before that partition is safe.

A nearest-target search cannot express these regimes: Tianzi is a window across temperature,
humidity, preserved relief and inlandness, and a Voronoi cell gives no terrain strength that
vanishes at its border. Add a new landform biome as a regime row, not as another special case
in the selection code.

`computeRegimeWeights` derives every regime's terrain strength from the same suitabilities in one
pass, returned with the natural terrain so consumers never recompute it: 0 at the label threshold,
1 at `fullStrength`. It is also multiplied down to 0 approaching the label of every
higher-priority regime, over that regime's `fadeWidth` just below its threshold. So a landform
never extends past its label, and overlapping regimes (red desert spires under Mesa) need no
special-case masks. `fadeWidth` must stay below the threshold: suitabilities bottom out at 0,
so a fade reaching below 0 suppresses lower regimes everywhere (this once capped red desert
spires at 31% height far from any mesa). Apply the ramp **after** combining every suitability axis:
separately fading inlandness let coastal columns keep tall pillars after the label had already
switched to tundra. Shape, formation rock and cliff soil use the unjittered strength; biome
jitter can affect the negligible outer foothills but cannot cut through a tower's core.

Swamp's row sets its label and how other regimes fade next to wetlands; its terrain comes from
flood cells with their own threshold (see [swamp_generation.md](swamp_generation.md)). The flood
factor's flatness term reads `ruggedWeight`, the same relief factor terrain uses, rather than a
separate erosion window.

A row may also set its landform's density amplitude (roughness). Roughness uses the regime's
*coverage* weight (full across the whole label, fading just outside it), not the landform ramp:
blending by the landform weight left the outer band of Mesa and red desert labels, where their
landforms are still weak, with full mountain roughness, which carved ravine-like gashes into
rugged dry ground. Elsewhere roughness follows relief alone. Roughness is not purely texture: terrain below the base
height is denser, so a larger amplitude raises the effective ground slightly. It therefore must
not follow raw climate, which is exactly what a dryness term once did.

Erosion controls suitability, not nearest-target distance. Its thresholds belong to shared
terrain regimes rather than individual biome height overrides. The swamp flood factor also
requires eroded terrain. Oases are spatial pond footprints and override the label in both the
chunk generator and map, after climate selection; a climate-only lookup cannot locate one. A
pond site is only active where no regime claims the label, so ponds never cut into terraces or
spires.
See [swamp_generation.md](swamp_generation.md) for the other local-water override.

## Per-Column Jitter

`BiomeNoise::randomOffset` adds tiny random offsets before selection. This softens biome boundaries — columns near an edge occasionally flip, creating a natural ragged border instead of a sharp line following an isosurface.

## BiomeData Role

Each biome's `BiomeData` bundles its noise target point, surface blocks, structure generators, and decorator. This is the single definition point for a biome's identity — adding a new biome means adding one entry to the enum and one initialization block.

Landform-specific rock is applied separately from topsoil. Mesa deliberately leaves its top
and mid blocks unset, preserving elevation-based terracotta bands instead of repainting every
column with the same cap. Append biome/structure enum values: world exports serialize them.
