_Last edited: 2026-09-21_

# Terrain profiles and local formations

The erosion axis is an artistic measure of preserved relief, not a hydraulic simulation.
Low values preserve dramatic formations, middle values favor terraces, and high values
reduce relief and permit wetlands. Peak still determines broad elevation and inlandness
still determines land/coast placement. Existing biomes acquire gentler foothills without
adding per-biome shaping fields; the terrace profile additionally requires dry climate.

The dry terrace regime blends a complete mountain profile into modest, connected plateaus. Warped
multiscale noise breaks up their outlines and cuts short gullies into escarpments; height
quantization supplies only secondary ledges. Remapping a smooth mountain into evenly spaced
steps produced huge concentric terraces, which the user's Minecraft references specifically
ruled out. Keep Mesa relief closer to low buttes than Tianzi's tall isolated pillars.

All formations retain the same continuous ground beneath them. Independently suppressing
mountain relief with terrace and Tianzi masks made deep troughs where the replacement relief
was absent or only partly weighted. Applying terraces in humid terrain also cut thin trenches
through ordinary mountains. Blend complete Mesa and ordinary profiles with complementary
weights. Ordinary preserved highlands additionally receive a steep peak-driven elevation
response and stronger broad 3D displacement. The extra relief grows toward the interior and
fades out before dry terrain; suppressing it globally had erased the tall ordinary mountains.
Tianzi exchanges this extra mountain relief for its complete stacked formation profile with
complementary weights, retaining the modest shared ground throughout. Only the broad density
amplitude grows in ordinary mountains; the fine detail field remains confined to formations.

`BiomeNoiseFields::computeNaturalTerrain` returns the same height and density amplitude
inputs consumed by the existing 3D threshold. It is a pure world-position query after seed
initialization, suitable for a future coarse height sampler. It is **not** the exact highest
solid voxel: surface noise, caves and local water shaping still follow it. Keep that distinction
when building distant terrain or pre-river drainage heights.

Small bumps come from a separate, bounded 3D detail field in chunk generation. A more irregular
2D outline alone still extrudes long, manufactured-looking walls. Slope compensation makes the
detail move the sides of cliffs as well as their tops. Its strength combines erosion with climate,
so it is confined to Mesa and Tianzi and fades out before reaching red desert or ordinary biomes.
Applying it to every low-erosion region made desert and savanna unnecessarily blobby. The capped
slope boost preserves small bumps without creating tall points along summit edges.
Mesa's detail also follows a smooth slope taper: gentle floors and plateau tops retain 35%
of its full strength, increasing toward full detail on escarpments. The existing natural
gradient supplies the taper, keeping it independent of chunk boundaries and the noise being
modulated. This changes fine surface texture without moving the broad plateaus or their strata.

## Shared formation pattern

`terrain_formation.h` generates deterministic, jittered sites with a broad foot and narrower
core. Tianzi uses broad summits; quartz uses narrow summits. Warm/dry climate smoothly trades
towers for spires, so these profiles do not abruptly change at the biome-label boundary.
Sites are world-based rather than chunk-based and sampled before voxel filling, which makes
the surrounding ground rise into each feature. A future island/karst profile can reuse this
pattern with a different base elevation and water treatment. Tianzi's climate, erosion and
inland suitability is combined before applying the formation-strength ramp. This makes the
strength vanish at every label boundary, including coastal edges, while ordinary mountain
relief returns with the complementary weight. Formation-rock coverage and cliff soil use
that same strength independently of the jittered label. Correlated coverage still leaves
some existing stone/marble patches around roots and foothills.

Tianzi divides the former two-tier core-height budget across three independently seeded site
fields, each with a finer footprint. The reusable sampler accepts an array of profiles; each
tier needs support from the preceding tier, so crowns cannot appear independently on valley
floors or skip a missing shoulder. Wider summit fractions and shorter lateral ramps make
steeper walls with broad plantable shelves at several elevations, rather than increasing
total height. Faceted footprints and variable site heights break up the tiers. All levels
share one continuous biome weight, so stacking does not add another transition regime.
Core footprints are widened independently of site spacing and root extent to avoid thin
needle-like towers while keeping distinct gaps and the same vertical tier budget. Check the
finite-support bound when widening: footprint warping grows with the core radius too.
Main sites are now farther apart, with smaller spacing increases in the upper tiers. This
opens valleys between formations without shrinking their cores or eliminating most stacked
crowns. Increasing every tier's spacing equally would make higher shoulders much rarer.

Steep Tianzi faces receive a little more bounded 3D displacement for shallow recesses and
overhangs. Positive displacement is separately capped near crowns: lifting the full cliff
slope boost above a summit produced thin tips and detached rubble. Recesses retain the larger
bound, and the existing conservative noise-sampling range encloses both sides. The cliff boost
is restrained around the narrower upper tiers; too much inward displacement shredded their
crowns into thin, bare teeth and removed useful planting area.

Quartz uses the sampler's angular profile: a rotated, faceted footprint and an exponential
rise toward a narrow crystal core. Removing the exponential's linear term joins the outer
foot to the desert with zero slope, then accelerates inward instead of rounding into a dome.
Its crystal and root dimensions are deliberately smaller than Tianzi's. The surrounding red
rock rises farther up the core before quartz begins; broad circular feet previously made
every crystal look planted on a separate mound.

Support must fit inside the 3x3 site scan even after footprint stretch and warp. The sampler
asserts that bound; exceeding it would create seams when the search window moves. Enlarging
a profile's radius may require a larger stencil. Keep height and amplitude blends in smooth
field space, never in the randomly jittered biome labels. Amplitude is the reciprocal of the
density multiplier; interpolate that reciprocal when blending local shaping.

## Exposed materials and vegetation

`surface_material.h` supplies elevation-based terracotta bands and formation rock, before
the ordinary topsoil pass. Absolute elevation keeps bands connected across adjacent columns;
a slow world-position offset bends them slightly. Quartz stays exposed through the topsoil
pass, and trees/cacti cannot anchor on it. Its material is determined before carving, so quartz
is excluded from cave air, cave skins and cave-decoration support. Deep rock beneath formations
remains available to cave biomes.

Tianzi pillars also exclude cave carving above their shared ground, with the seal fading
into the roots below that height. This follows formation geometry independently of the
formation-rock coverage mask. Preserve the existing underground rock and skin selection: stone
and marble outcrops in the lower transition are intentional, even where no cave opens there.

Mesa's palette follows the user's mega-minecraft project, with brown replacing the original
purple at the user's request. Strata average three blocks thick, with irregular widths and no
fixed repeating color sequence; white and brown are thin accents. Keep the material frequency
independent of terrace spacing: many strata should cross a single escarpment. Irregular seeded terrace intervals, differing ramp
widths and residual shelf slope avoid a stack of identical, flat treads. Terracotta,
red sand/sandstone and quartz textures were copied from the user's GoodVibes block directory
(`C:/Users/SDOAJ/code/textures/GoodVibes/minecraft/textures/block`). Tianzi currently uses regular
stone pending a dedicated weathered rock texture; yellow smooth sandstone made the towers
read as desert terrain. Tianzi's trees/shrubs use the existing pine log and leaf
assets; their generators obey the same clipping-independent RNG contract as other trees.
Tianzi's broad ledges, summits and valley floors receive soil, while steep faces retain
exposed rock. Lower shelves beneath an overhang can also acquire a grass cap when there is
enough open headroom above the shared ground. Do not scatter soil over tiny steps on the
steep faces merely to permit trees: the grass/dirt sides stood out as colored speckles.
Tianzi's structure rule instead accepts exposed stone as well as grass, independently of
the soil pass. Keep the cave-air exclusion so accepting stone does not plant cave floors.
Pines and shrubs use the reusable [exposed-surface placement](structure_system.md)
rule: enumerate eligible ledges first, then select fitting plants with actual support, clearance
and 3D spacing. This replaces both the sparse XZ sampling and blanket vertical gap; narrow
shelves can carry shrubs and larger shoulders can support pines. Tianzi checks a clear
trunk column with solid root support while allowing foliage to meet the backing cliff;
requiring a full ring of air at root level rejected most narrow steps. Soil creation remains in
terrain generation, rather than having a plant-placement rule repaint its own supports.
Boundary slope samples use the same world-space natural terrain query as interior cached
columns, avoiding a special edge treatment at chunk borders.

## Pond oases

`oasis_shaping` caches seeded pond sites for the requested region and halo. Eligibility uses
smooth hot/dry inland fields at the site. Each pond has one integer water level, a depressed
floor, a fully raised rim, and an outer blend to natural terrain. The water-level override
ends inside that rim, before terrain blending begins; surface noise is suppressed there.
This contains elevated water without simulating flow. Nearby caves are sealed around the
waterline using the existing swamp mechanism, leaving deeper caves intact.

Each site combines a larger basin and smaller offshoots with a seeded orientation, proportions
and shoreline distortion. This creates bays and peninsulas at the scale of the pond; small noise
on one ellipse still looked like an artificial oval. Depth, bank height and vegetation width
vary separately, while the bowl, water cutoff and raised rim share one distorted distance.
Separate water and terrain masks would expose water at the irregular inlets.
Shoreline noise is relative to its value at the site and remains bounded: a uniform positive
noise bias otherwise shrinks every basin into similar round pools instead of bending the shore.

Site spacing bounds the entire basin cluster and distortion so neighboring oases cannot
overlap. The support bound includes basin-center offsets, radii, smooth-union expansion,
shore noise, domain warp, aspect ratio and the outer blend. If changing any of these, recheck
that invariant as well as the halo.
Both `fillBiomeRect` and chunk generation query the same footprint for vegetation/tints;
do not approximate oasis labels using climate alone. River interaction is deferred.

## Validation

For changes to these profiles, inspect rendered boundaries across several seeds and test
whole-region versus chunk-local queries, including negative coordinates. Check actual pond
voxels for exposed lateral/bottom water faces and regenerate chunks in a different order.
Biome-label maps alone cannot establish shape continuity or water containment. Temporary CPU
harnesses belong under ignored `build/`; see [CPU experiments](../tests/cpu_terrain_benchmarks.md).
