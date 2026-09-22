_Last edited: 2026-09-20_

# Terrain-guided rivers

## Direction and timing

Add connected rivers that follow the existing landscape, merge into larger
channels, and drain toward defined outlets. Prefer a coarse drainage plan followed
by localized valley/channel shaping over a global terrain rewrite or independent
noise channels filled at sea level.

Defer river implementation until a shared coarse surface sampler exists. This is
also a prerequisite for the planned heightfield-only terrain used by distant
LoDs, but rivers do not need to wait for LoD rendering itself. The common dependency
is cheap, deterministic terrain queries over large areas without generating voxel
chunks.

The current biome work can proceed first: soft terraces for Mesa, a local pillar
field for Tianzi Mountains, terrain-integrated quartz spikes with broad raised
bases in Red Desert, and contained pond oases. Oasis/river interaction is a later
design task, not part of the initial pond implementation.

## Shared terrain description

Keep three distinct stages:

1. **Pre-river landscape.** Describe the existing terrain and its local biome
   shaping independently of rivers. Expose surface estimates, coast information,
   and existing water-body levels/footprints where applicable.
2. **Drainage plan.** Query that landscape on a fixed world-space grid and produce
   river connectivity, paths, channel sizes, bed profiles, and water elevations.
3. **Final landscape.** Apply the river plan to both detailed voxel generation and
   the surface representation used by distant LoDs.

The drainage planner must never read back its own final carved terrain as a new
input. Keeping the stages separate avoids circular dependencies and repeated
rerouting. Swamp and oasis metadata should be available without depending on a
finished river plan; decisions about their eventual connections belong to the
drainage stage.

`BiomeNoiseFields::computeNaturalTerrain()` is a useful starting point, but its
`baseHeight` is not the final surface height. The 3D density field can move the
surface substantially and form overhangs. A coarse sampler must approximate the
exterior surface, potentially through sparse density evaluation, rather than
presenting the base height as ground truth. Near channels, refine or validate the
coarse estimate against the detailed surface so small obstructions do not interrupt
the river.

Drainage and LoD rendering can use different resolutions of the same terrain
description. Drainage needs a stable broad landscape; rendering needs a convincing
surface envelope. A heightfield cannot preserve every cave or overhang, but it
should agree on coastlines, dominant relief, and water elevations. Narrow Tianzi
pillars and quartz spikes need feature-aware or conservative sampling so coarse
render grids do not simply miss their silhouettes.

## Drainage planning

Plan on a coarse grid or graph covering areas much larger than a chunk. Candidate
paths should favor existing valleys and low passes, preserving terrain away from
river corridors. Select sources, accumulate upstream contributions, and use that
accumulation to control channel width and depth. Tributaries should merge, and
shared downstream segments should have one consistent profile.

A local downhill walk is insufficient: it gets trapped in depressions and cannot
decide whether to form a lake, cross a saddle, or continue to a distant outlet.
Explicitly resolve basins by retaining selected lakes with spill elevations or
breaching a route where the terrain change is acceptable. Specify meaningful
outlets, normally the ocean or a planned lake; allow closed inland basins only as
an intentional feature. Bound excessive excavation rather than turning every
mountain crossing into a canyon down to sea level.

World streaming makes connectivity the main architectural challenge. Before
implementing the planner, choose how drainage regions share outlets and routes:
for example, a deterministic hierarchy of large regions with agreed boundary
connections, followed by local route refinement. A fixed local halo alone does not
guarantee globally correct drainage. Do not assume independent per-region solves
will join automatically.

Region plans may be generated lazily and cached, but their results must depend
only on world seed, coordinates, and generator settings. Chunk generation order,
thread scheduling, camera position, render distance, and active LoD must not change
river paths. LoDs simplify the same planned rivers; they never plan new ones.

## Channel shaping and water

Represent each planned reach with a downstream profile and a cross-section:
water elevation, bed depth, channel width, and a wider valley influence. Water
elevations must be non-increasing downstream, and tributaries must meet the main
channel at compatible levels.

Apply the profile through the existing terrain density system before voxel fill.
Blend the broader valley into the original landscape and suppress enough local
surface variation to keep the channel continuous. Outside the river's finite
influence, retain the existing terrain calculation. Derive all shaping from smooth
world-space fields, not jittered per-column biome IDs.

Biomes can change the cross-section and materials without changing connectivity:
gentle grassy banks in plains, exposed terracotta canyon walls in Mesa, and sandy
banks in deserts. This gives Mesa rivers from the shared drainage system rather
than a separate biome-specific network.

Use generated static water initially; a running fluid simulation is not required.
With voxel water, descending reaches may use steps, rapids, or explicitly generated
waterfalls. Existing flat `WATER_TOP` surfaces are not by themselves a complete
solution for exposed drops, so verify the meshing and shading of those transitions.

Contain water laterally and protect riverbeds/banks from nearby cave openings.
Reuse the swamp system's ideas for local water levels and bounded cave sealing,
while preserving deeper caves. Swamp dams and future oasis outlets need an explicit
connection policy before rivers cross them; generic carving through their rims can
expose water at incompatible levels.

## Integration boundaries

- Retain existing noise seeds and the default density calculation. New effects
  should be local to the selected biomes and river corridors.
- Evaluate shape changes before deriving terrain sampling bounds and filling
  blocks, so tall formations and lowered valleys remain inside valid noise ranges.
- Share river bed, shoreline, and water-level data between voxel terrain and LoDs
  to avoid rivers disappearing or changing elevation across the detail boundary.
- Keep coarse planning independent of block allocation and mesh generation. Cache
  regional plans and batch terrain queries; full voxel generation must not be a
  prerequisite for planning distant routes.
- Treat full hydraulic erosion and sediment transport as outside this proposal.
  The goal is coherent generated drainage with controlled, localized terrain edits.

## Suggested sequence and validation

1. Add the selected biomes with terrain formations that can be queried separately
   from voxel generation.
2. Build the shared coarse surface sampler and compare it with detailed generated
   terrain across mountains, coasts, terraces, pillars, and ponds.
3. Prototype regional drainage on maps before adding voxel carving. Resolve the
   boundary/outlet strategy and inspect basins, confluences, and downstream profiles.
4. Add valley/channel shaping, bank materials, water filling, and cave protection.
5. Integrate the same plan into distant LoDs and later design swamp/oasis connections.

Validate determinism across region boundaries, negative coordinates, load orders,
and detail levels. Check that wet channels remain continuous, water is supported,
confluences agree on elevation, outlets meet their receiving water bodies, and
terrain outside the influence remains unchanged. Measure coarse query/planning
cost and cache memory before expanding the planning horizon.

## References

- [Current terrain shaping](../knowledge/terrain/chunk_generator.md)
- [Existing swamp water containment and cave sealing](../knowledge/terrain/swamp_generation.md)
- [Shared biome/noise sampling used by BiomeScanner](../knowledge/terrain/biome_scanner.md)
- [Génevaux et al., Terrain Generation Using Procedural Models Based on Hydrology (2013)](https://doi.org/10.1145/2461912.2461996)
  — background on graph-based drainage and terrain carving, not a drop-in solution
  for deterministic streaming across an unbounded world.
