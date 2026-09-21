_Last edited: 2026-09-20_

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
weights; add Tianzi formations on top of shared ground without lowering that ground first.
Broad mountain uplift is restrained so a bare ridge between Mesa and Tianzi does not tower
over the Mesa merely because neither specialized profile applies there.

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
pattern with a different base elevation and water treatment. Tianzi strength tapers almost to
zero before either its erosion or climate label boundary. The erosion ramp spans a broad band
so this becomes a sequence of lower formations, not a single steep row at the edge; the same
mountain ground continues underneath as pillars shrink into foothills. Correlated sandstone patches also thin out across that band, rather
than letting a tall pale pillar meet a dark mountain along a sharp material boundary.

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
sandstone coverage mask. Preserve the existing underground rock and skin selection: stone
and marble outcrops in the lower transition are intentional, even where no cave opens there.

Mesa's palette follows the user's mega-minecraft project, with brown replacing the original
purple at the user's request. Strata average three blocks thick, with irregular widths and no
fixed repeating color sequence; white and brown are thin accents. Keep the material frequency
independent of terrace spacing: many strata should cross a single escarpment. Irregular seeded terrace intervals, differing ramp
widths and residual shelf slope avoid a stack of identical, flat treads. Terracotta,
red sand/sandstone and quartz textures were copied from the user's GoodVibes block directory
(`C:/Users/SDOAJ/code/textures/GoodVibes/minecraft/textures/block`). Smooth sandstone reuses
the existing sandstone-top tile. Tianzi's trees/shrubs use the existing pine log and leaf
assets; their generators obey the same clipping-independent RNG contract as other trees.
Steep Tianzi slopes retain exposed rock; topsoil and pine anchors are limited to gentler
ground and summits. Boundary slope samples use the same world-space natural terrain query
as interior cached columns, avoiding a special edge treatment at chunk borders.

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
