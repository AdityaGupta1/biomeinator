_Last edited: 2026-09-22_

# Terrain profiles and local formations

The erosion axis is an artistic measure of preserved relief, not a hydraulic simulation.
Low values preserve dramatic formations, middle values favor terraces, and high values
reduce relief and permit wetlands. Peak still determines broad elevation and inlandness
still determines land/coast placement. Existing biomes acquire gentler foothills without
adding per-biome shaping fields; the terrace profile additionally requires dry climate.

**Terrain chooses the biome, not the other way around.** Elevation comes only from peak,
erosion and inland. Climate and regime weights select landform *styles* (terraces, towers,
spires, surface roughness) but never raise or lower the shared ground. Gating relief by dryness
flipped elevation by 100+ blocks wherever humidity crossed the dry threshold, cutting steep pits
where mesa or red desert met mountains. Dry regions are therefore as tall as the relief fields
make them: high peak in a hot, dry area gives tall terraced massifs or red desert mountains.

Mesa reshapes the shared elevation within bounded offsets rather than replacing it. Warped
multiscale plateau noise adds buttes and cuts short gullies (about +/-21 blocks), then partial
height quantization forms shelves, moving a column by at most one terrace band. Because every
Mesa term is bounded relative to the ordinary height, its regime ramp cannot open a pit into
neighboring relief. Shelf elevations shift regionally with a slow world-position noise, not with
climate, which would move every shelf whenever the climate fields are rescaled or equalized.
Keep quantization secondary: remapping a smooth mountain into evenly spaced
steps produced huge concentric terraces, which the user's Minecraft references ruled out.

All formations retain the same continuous ground beneath them. Independently suppressing
mountain relief with terrace and Tianzi masks made deep troughs where the replacement relief
was absent or only partly weighted. Preserved highlands receive a steep peak-driven elevation
response and stronger broad 3D displacement, growing toward the interior. Tianzi exchanges
this extra mountain relief for its complete stacked formation profile with complementary
weights, retaining the modest shared ground throughout. Only the broad density amplitude grows
in ordinary mountains; the fine detail field remains confined to formations.

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
core. Tianzi uses broad summits; red desert quartz uses narrow summits. Both are scaled by their
[regime weight](biome_system.md), so each fades out at its own label boundary.
Sites are world-based rather than chunk-based and sampled before voxel filling, which makes
the surrounding ground rise into each feature. A future island/karst profile can reuse this
pattern with a different base elevation and water treatment. Tianzi's climate, erosion and
inland suitability is combined before applying the formation-strength ramp. This makes the
strength vanish at every label boundary, including coastal edges, while ordinary mountain
relief returns with the complementary weight. Cliff soil follows that continuous strength.
Tianzi's material shell covers every column Tianzi covers (coverage at least 0.5, which includes
all of its formations), so unrelated cave stone and marble cannot show through its sides.

Tianzi splits its core-height budget across three independently seeded site fields, each with
a finer footprint. The reusable sampler accepts an array of profiles; each
tier needs support from the preceding tier, so crowns cannot appear independently on valley
floors or skip a missing shoulder. Wider summit fractions and shorter lateral ramps make
steeper walls with broad plantable shelves at several elevations, rather than increasing
total height. Faceted footprints and variable site heights break up the tiers. All levels
share one continuous biome weight, so stacking does not add another transition regime.
Core footprints are widened independently of site spacing and root extent to avoid thin
needle-like towers while keeping distinct gaps and the same vertical tier budget. Check the
finite-support bound when widening: footprint warping grows with the core radius too.
Main sites are spaced wider than the upper tiers, whose spacing grows less. This opens valleys
between formations without shrinking their cores or eliminating most stacked crowns. Increasing every tier's spacing equally would make higher shoulders much rarer.

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
every crystal look planted on a separate mound. The entire crystal uses smooth quartz;
the sandstone foot remains part of the terrain below it.

Support must fit inside the 3x3 site scan even after footprint stretch and warp. The sampler
asserts that bound; exceeding it would create seams when the search window moves. Enlarging
a profile's radius may require a larger stencil. Keep height and amplitude blends in smooth
field space, never in the randomly jittered biome labels. Amplitude is the reciprocal of the
density multiplier; interpolate that reciprocal when blending local shaping.

## Exposed materials and vegetation

`surface_material.h` supplies elevation-based terracotta bands and formation rock, before
the ordinary topsoil pass. Absolute elevation keeps bands connected across adjacent columns;
a slow world-position offset bends them slightly. Quartz stays exposed through the topsoil
pass, and trees/cacti cannot anchor on it. Smooth quartz is recognized before carving and
topsoil/structure placement, so it cannot acquire caves, skins, soil or plants. Because quartz
controls the carve mask, it follows the red desert landform weight itself. Yes/no rock
materials (terracotta, the red sandstone shell, Tianzi strata) follow their regime's coverage via
`NaturalTerrain::isCoveredBy` (coverage at least 0.5) rather than the label: per-column jitter
otherwise alternated materials along one cliff at a regime border. A cutoff above 0 matters:
coverage starts well below the label threshold, and testing `> 0` spread terracotta over an extra
area about a fifth the size of Mesa itself.

Terracotta strata are seeded per world. The layer sequence is a table built once at generator
init rather than hashed per voxel; its range covers every height plus the largest bedding offset,
which the Mesa column asserts.
Deep rock beneath formations remains available to cave biomes.

Tianzi pillars also exclude cave carving above their shared ground, with the seal fading
into the roots below that height. Material replacement extends beneath the conservative
lowest possible surface, so exposed recesses and roots keep the Tianzi palette. Underground
cave rock and skins below that shell retain their normal classification.

Tianzi's broad strata are seeded by the dominant supporting Worley site, whose identity is
returned with the natural-terrain query without changing its height. Between feet, where no site
contributes, the nearest site owns the column; the search cell would switch owners along straight
grid lines and seam the bedding. That search covers 5x5 cells, not the 3x3 used for height:
stretch and warp can make a site outside the 3x3 window measure nearest, which also seams along
the grid. Strata follow Tianzi coverage, not the label. Stacked shoulders share
that foundation's geology, while neighboring pillars get different phases and layer sequences.
Each stratum is 20–40 blocks thick; adjacent materials always differ so two layers cannot
merge into an unintended double-width band. Warped, jittered fracture cells offset whole
pieces of the bedding, giving contacts abrupt height jumps within a pillar rather than only
gentle waves. Throws are amplified independently of the fine noise, preserving quiet interiors
between the larger jumps. Smaller-scale warp makes the fracture outlines jagged; mild dipping and chipped
edges break up the remaining horizontal runs. The same displacement applies to every boundary
in a column, preserving the 20–40-block thickness and preventing layer crossings. The field is
world-based, so fracture edges do not follow chunk boundaries. Darker, world-space 3D mineral patches
cross these bands. Their XZ-interpolated noise planes are cached within each column; sampling
order never affects the result. The material fields do not displace terrain.

Mesa's palette follows the user's mega-minecraft project, with brown replacing the original
purple at the user's request. Strata average three blocks thick, with irregular widths and no
fixed repeating color sequence. Plain and orange terracotta form the bedding, with plain
favored. Other colors appear only as 1–2-block seams at either edge of occasional strata.
Neighboring seams may touch or have base material between them; there is no enforced gap.
Full-height accent strata had dominated exposed slopes despite their modest selection
probability. Each seam is capped at its containing layer's width so short strata do not
overwrite their neighbors. Adjacent accents of the same color can still merge visually.
Two scales of seeded XZ noise displace the whole stack by up to four extra blocks,
preserving layer thickness and the deep-rock boundary. Keep the material frequency
independent of terrace spacing: many strata should cross a single escarpment. Irregular seeded terrace intervals, differing ramp
widths and residual shelf slope avoid a stack of identical, flat treads. Terracotta,
red sand/sandstone textures were copied from the user's GoodVibes block directory
(`C:/Users/SDOAJ/code/textures/GoodVibes/minecraft/textures/block`). Tianzi's selected T03/T05/T07
textures provide the strata and T06 supplies darker patches. Quartz uses Q01 throughout
the crystal, replacing the bordered GoodVibes quartz faces. Source mappings are kept in the texture
folder's `formation_sources.md`; Yuushya Q01/T07 remain under `textures/yuushya/` with that
folder's separate attribution and license. Tianzi's trees/shrubs use the existing pine log
and leaf assets; their generators obey the clipping-independent RNG contract.
Tianzi's broad ledges, summits and valley floors receive soil, while steep faces retain
exposed rock. Lower shelves beneath an overhang can also acquire a grass cap when there is
enough open headroom above the shared ground. Do not scatter soil over tiny steps on the
steep faces merely to permit trees: the grass/dirt sides stood out as colored speckles.
Tianzi's structure rule instead accepts all its dedicated rock types, ordinary stone and
grass independently of the soil pass. Lower-shelf soil also recognizes the complete rock
family. Keep the cave-air exclusion so accepting rock does not plant cave floors.
Pines and shrubs use the reusable [exposed-surface placement](structure_system.md)
rule: enumerate eligible ledges first, then select fitting plants with actual support, clearance
and 3D spacing. This replaces both the sparse XZ sampling and blanket vertical gap; narrow
shelves can carry shrubs and larger shoulders can support pines. Tianzi checks a clear
trunk column with solid root support while allowing foliage to meet the backing cliff;
requiring a full ring of air at root level rejected most narrow steps. Soil creation remains in
terrain generation, rather than having a plant-placement rule repaint its own supports.
Boundary slope samples use the same world-space natural terrain query as interior cached
columns, avoiding a special edge treatment at chunk borders. The columns just outside the chunk
are batched through `fillPositions` rather than sampled one point at a time.

## Pond oases

`oasis_shaping` caches seeded pond sites for the requested region and halo; sites farther than
their support from the region skip their noise evaluation entirely. Eligibility uses smooth
hot/dry inland fields and the shared relief factor at the site, and excludes ponds any terrain
regime claims anywhere in the footprint (checked at the center and on two rings): a pond forces
an absolute floor and bank, which would cut into Mesa terraces or red desert spires, and scaling
it down near one would breach its water containment. The water level comes from the ground before formations, so a pond never sits
on a butte or spire foot. Each pond has one integer water level, a depressed
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
