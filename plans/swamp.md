# Swamp Plan

Inland swamp biome with standing water above sea level, cypress trees in/around the water.
Reference: mega-minecraft's SHREKS_SWAMP (terrain straddles waterline, mud ring, cypress with
flared trunk + drooping leaves). Expands on world_overhaul.md item 5 Stage A/B.

## Ground truth (verified)

- Water created in exactly one place: `chunk_generator.cpp:663-666`, `else if (y <= seaLevel)`
  on the `!isInTerrain` branch. `SEA_LEVEL 125` in `common_settings.h:48`.
- Everything downstream is already water-level-agnostic — water lives in block data:
  - Waves snap to any integer level (`water_displace.cs.hlsl:32`: `restY = round(y - 0.875) + 0.875`).
  - Camera-underwater reads the actual block at the camera (`terrain.cpp:203-222`).
  - Underwater shading = per-crossing payload flags, no plane assumption. No planar reflection.
  - Structure underwater rejection reads blocks (`chunk_generator.cpp:800-807`);
    `tryPlaceStructureBlock` already overwrites WATER/WATER_TOP so trees can stand in water.
  - `STRUCTURE_GEN_FLAG_ALLOW_UNDERWATER` (`structure.h:43`) exists but is unused/untested.
- One real meshing bug: `chunk.cpp:544` — water side faces emit only against AIR. Full WATER cube
  next to a lower WATER_TOP leaves an unmeshed 0.125 strip (see-through lip). Solid blocks already
  handle this exact case at line 528; water needs the mirror rule. ~1 line.
- Caves stay dry by construction: cave carve is on the `isInTerrain` branch, water on the `else`.
  Any change must keep the water fill on the `!isInTerrain` branch or lakes drain into caves.
- Terrain shaping is 100% biome-independent today (smooth noise fields only). The per-column biome
  is jittered (`BiomeNoise::randomOffset`) — deriving any height/level from it gives adjacent
  columns different values = 1-block cliffs. All swamp shaping must come from smooth fields.
- Lowland band has only 4 biomes; the hot+wet corner is empty. SWAMP at roughly
  (temp 0.5, humidity 0.8, peak -0.8) slots in with clean Voronoi separation.
- `heightfield` is thread scratch, discarded after structure placement. The water table must be
  analytic (pure function of XZ) so any consumer can recompute it statelessly.

## Design: analytic per-column water table

`waterLevel(x, z)` computed alongside `terrainBaseHeight` in `fillTerrainBlocksAndCreateStructures`.
Swamp mask = smooth function of temperature/humidity/peak fields (hot + humid + flat), optionally
times one new dedicated mask noise. Containment by ordering: terrain is forced below the table only
where the mask is high; as the mask fades, terrain rises above the table (shoreline) while the table
is still at swamp level; only further out does the table drop to seaLevel, where no water exists.

Touch points, all in one function (`chunk_generator.cpp`):
1. Base-height forcing + surface-multiplier flattening from swamp mask (~lines 404-419, same mix
   trick as the coast `seaLevelPullFactor`).
2. `maxFillY` (line 467) uses the chunk's max water level instead of `seaLevel`.
3. Water fill (lines 663-666): per-column `y <= waterLevel[columnIdx]`, WATER_TOP at equality.
4. Top-blocks pass (~lines 706-720): mud ring where `topBlockY` is within ~1 of the column water
   level (above and below the waterline). Needs a MUD block.

Water surface is flat per pond; all height variation comes from the terrain under it (hummocks poke
through where ground crosses the table).

## Stage 1 — single global swamp level

`waterLevel = SWAMP_LEVEL` (a few blocks above sea, e.g. ~130) where mask high, `seaLevel` elsewhere.
No steps anywhere by construction — no terrace or lip artifacts possible. Constraint: surrounding
lowland terrain in the mask fade band must naturally exceed SWAMP_LEVEL (lowland base runs
~seaLevel+8 and up, so a level slightly above sea fits; high-altitude swamps don't).

Content that lands with this stage:
- SWAMP biome entry (`biome.cpp`): noise target in the empty hot+wet+flat corner, top blocks,
  grass tint, decorators (dense grass/fern/flowers on swamp grass).
- MUD block: enum append (serialized — append-only), BlockData, texture.
- Cypress tree (below), first real use of `ALLOW_UNDERWATER` — validate wet-trunk meshing and
  decorator interactions around water.

## Stage 2 — cellular ponds (the look we want)

Swap the constant for a cellular table; everything downstream unchanged. Don't detect basins —
manufacture them. This is also world_overhaul.md Stage B (alpine lakes) machinery: same code,
highland mask, deeper bowl, stronger rim.

Per column (analytic, chunk-seamless):
1. Hand-rolled worley over a jittered sparse grid (3x3 neighborhood scan, same pattern as
   `gridCellCornerForPosXZ_WS`): nearest site F1, second-nearest F2, both site IDs.
   Hand-rolled, not FastNoise2 — need site IDs and both distances, not just F1.
2. Pond level per cell: `L = baseLevel + floor(hash(siteID) * range)`, integer.
3. `edge = F2 - F1`: interior (large) → force terrain below own L (depth profile shelving toward
   shore); border (small) → raise terrain to `max(L_own, L_neighbor) + margin` = dry rim.
   Knowing the neighbor's level is why the worley is hand-rolled.
4. Domain-warp the input coords with simplex before the cell scan — kills polygonal shorelines.
5. Merged marshes for free: quantize levels coarsely (2-3 possible values); when neighbors hash to
   the same level, let the rim drop → cells fuse into large irregular marshes. Different-level
   neighbors keep their bank.
6. Flood factor = min(interior factor, swamp mask) so ponds near the swamp edge dry out instead of
   leaking into neighboring biomes.

Cannot downsample + interpolate this (interpolating cell IDs is nonsense) — exact per-column eval,
but the 3x3 scan is cheap.

Knobs: cell size (100-300 blocks), rim width (edge threshold), level variance, depth profile.
Fiddly part is the rim: too narrow → leaks at 3-cell corners (widen margin or consider F3);
too wide → reads as pond-dotted plain instead of marsh. Coarse level quantization (merged cells)
is the main lever.

## Lip meshing fix (do first, standalone)

Mirror the line-528 solid rule for WATER at `chunk.cpp:544` so water side faces emit against a
lower neighboring WATER_TOP. Golden-testable on its own. Stage 1 doesn't strictly need it and
Stage 2's rims mostly avoid it, but merged-cell borders and mask-edge geometry can still produce
near-touching water — it's the safety net.

## Cypress tree

Mega-minecraft's is SDF-style (per-block membership test on GPU); ours is imperative stamping —
mechanical inversion, `placeLeafCap` is already their `jungleLeaves` turned inside-out.

| mega-minecraft | biomeinator |
|---|---|
| Cone leaf crown + per-branch leaf cones | `placeLeafCap`, direct 1:1 |
| 6-10 spiral branches (rasterized lines, descending heights, angle += 90-270°) | `fillLine`, same as acacia/large-oak; loop arithmetic ports verbatim |
| Flared trunk, radius(t) = `0.5*((1.3+t)/(0.73+t)^4)+0.5`, y from -2 | new `placeTaperedColumn` helper (~30-40 lines, disc loop like `placeLeafCap` with caller radius profile); base sunk to -2 like LARGE_OAK |
| Simplex wobble on base radius | optional: small hash-based value noise seeded once per structure, or skip |
| Leaf droop (20% of columns pull leaves down up to 2) | droop variant/params on `placeLeafCap` (~15-25 lines) |

- Canopy reach ~10-12 blocks → `StructureBounds` ≈ 12, same as PALM_TREE; fits
  `structureMaxChunkRadius = 1`. Height is a non-issue (full-height chunks).
- RNG stream invariant: trunk/branch draws are unconditional — safe. Per-column droop and trunk
  wobble must NOT draw from the rng inside chunk-clipped loops — draw one seed up front, then hash
  (seed, dx, dz) per column.
- All wood (trunk + every branch) stamped before any leaves — first-placed-wins, or leaf cones
  block later branch lines.
- Registration: `StructureType` append (serialized — append-only), fill fn + bounds in
  `Structures::init()`, CYPRESS_WOOD/CYPRESS_LEAVES blocks + textures.
- Swamp gen roughly: cypress on grid ~18/padding 3 with `ALLOW_UNDERWATER`, sparse birch inland,
  dense decorators.

## Risks

- Deriving any level/height from the jittered per-column biome → 1-block cliffs. Smooth fields only.
- Water fill drifting off the `!isInTerrain` branch → flooded caves.
- Deep flooding + cave breach → dry cave under meshed water ceiling (no hole, just wrong). Keep
  ponds shallow (1-3 blocks); near-surface cave suppression protects the floor.
- `ALLOW_UNDERWATER` path untested; validate meshing/decorators around wet trunks in Stage 1.
- Fog density profile anchors at SEA_LEVEL (`fog.hlsli:16`) — cosmetic only; per-biome fog is
  overhaul item 2, ignore here.
- Goldens: every stage regolds terrain-dependent tests.

## Build order

1. Lip meshing fix (`chunk.cpp:544`), regold.
2. Stage 1: swamp mask + SWAMP biome + MUD + global SWAMP_LEVEL + mud ring.
3. Cypress tree + `ALLOW_UNDERWATER` + decorators.
4. Stage 2: cellular ponds (worley table + rims + merged cells).
5. Later, shared with overhaul: alpine lakes reuse Stage 2 machinery; per-biome water sigmaA/fog
   color from overhaul item 2 gives swamp its murky water.
