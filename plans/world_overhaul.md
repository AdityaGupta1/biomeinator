# World Overhaul Plan

## 1. Grass tint + biome GPU plumbing (do first, unlocks everything)

- Nothing biome-related on GPU today. Two paths:
  - `PerTriangleData.pad0` — free 4 bytes, packed RGBA8 tint written at mesh time from `Chunk::biomes[columnIdx]`.
  - World-XZ biome color texture (low-res, clipmap-style scroll with camera) — needed anyway for fog/miss rays. Bindless SRV, free `HeapIndices` slot. Waves already do world-XZ lookup.
- Gray out green grass texels, mask channel says "tint here". Shader: `lerp(1, biomeTint, mask)` in `getMaterialBaseColor`. `TexSampleCtx` built at only 4 sites, all have `perTriData`. ~4 line shader change.
- Emissive repack (strength + grass mask in one aux texture, emissive color into diffuse): removes `emission.png`, one fetch instead of two. Blockers: texture pipeline hardcodes sRGB + mip gen stomps alpha — need linear `loadTexture` variant. Risk: `trySplitMaterial` clears emissive texture ID on split — must preserve aux emissive or silently lost.
- Per-column tint = hard biome edges. Smooth: average neighbor columns at mesh time, or bilinear on world-XZ texture.
- Goldens need regold.

## 2. Water color + fog color per biome

- `waterSigmaA` hardcoded in `water.hlsli`. Make per-biome: lookup via water triangle biome index, stash in payload at entry (free pads). Camera-underwater already CPU-resolved — upload camera sigmaA next to `cameraUnderwater`.
- Brown varzea / black igapo / blue ocean = just different sigmaA.
- Fog color cheap: extinction stays gray scalar, closed-form transmittance untouched. Color only in source terms — one multiply each. Sample world-XZ biome texture per segment.
- Fog density: full spatial variation breaks closed form (O(N²) march). Separable density = sigma(y) × s(worldXZ), s once per segment — closed form survives. Heavy-fog cloud forest works this way.

## 3. Trees, shrubs, vines

- New tree ~30-60 lines with `fillLine`/`buildSpline`/`placeLeafCap`. Kapok (splayed buttress splines), jacaranda/piuva (acacia-style cap, colored leaves), acai (thin palm), rubber/brazilwood/mahogany (oak variants).
- `structureMaxChunkRadius = 1` caps canopy ±16 blocks XZ — giant kapok near limit.
- Forest fix: multiple `StructureGen` per biome already supported — big trees sparse grid + understory dense grid. Decorator: add ferns, multi-block tall grass (loop owns column, easy).
- Vines: no blockstates. Orientation in Block enum — `VINE_XPOS/XNEG/ZPOS/ZNEG`, 65k free IDs, zero memory. New `BlockShape::WALL_MOUNTED`: third mesher branch, one quad nudged 1/16 off wall, ~40-60 lines.
- Place vines from tree generators, NOT decorator (decorator stage neighbor reads = data race). `tryPlaceStructureBlock` skips obstacles without stopping — vine run needs own loop, break at first non-AIR.
- Same shape covers moss, lichen, trunk orchids later.

## 4. New biomes (data waves)

- Biome = enum entry + init block in `biome.cpp`. `uint8_t`, up to 255 fine.
- Crowding: lowland band gets full. Need more inland bands or extra selection axis (weirdness) — varzea/igapo/terra firme all hot+humid+flat.
- Terrain chooses the biome, not the other way around (see `knowledge/terrain/biome_system.md`). Don't add per-biome height offsets or swap profiles; a new landform is a bounded style on top of the shared height (like mesa terraces), and its label derives from the same factor. Landform biomes go in the terrain regime table; others are climate targets.

### Highland and high-ground biomes

Highland candidates are chosen purely by relief (`highlandReliefWeight`), and the highland list holds only relief biomes: today just mountains. Savanna and ice fields are climate zones and live in the lowland list. So the new mountain-country biomes go in the **highland** list, each a climate target within relief ground:

| climate | biome | character |
|---|---|---|
| warm, very humid | cloud forest | mossy twisted trees, vines, dense undergrowth, heavy fog (fog density plumbing in §2) |
| cold, humid | taiga / boreal | dense spruce, snow near the top of the range |
| cool, moderate | highland moor / alpine meadow | grass, heather, flowers, boulders, sparse conifers |
| dry (hot or cold) | high desert steppe | sagebrush, junipers, sparse grass; dry relief ground mesa/red desert don't claim |
| cold, dry / very high | mountains (existing) | bare stone peaks |

Redwood forest is the exception: mild, very humid, **lowland** and coastal (tall thick trunks, ferns, fog), so it's a lowland climate target, ideally one that favors ground near the coast.

Cloud forest shares Tianzi's warm/humid climate; they separate by erosion (Tianzi takes preserved relief as a regime, cloud forest the eroded rolling highlands left over).

Remaining lowland issue: lowland climate targets all have negative peak, and `distance2` weighs peak like climate, so high-peak lowland (eroded high ground, or high ground near the coast) picks whichever low-peak target is least wrong (e.g. forest on hot, dry ground at peak ~0.96, seed 100 x≈3863). Since relief already decides lowland vs highland, the simplest fix is for the lowland search to ignore peak; decide alongside `plans/biome_balancing.md`.

### Seaside cliffs (Big Sur)

A coastal landform, with the label derived from it:

1. `cliffWeight` from high peak + low erosion near the coast.
2. Let relief reach the shore: two terms flatten every coast today — `coastPull` (toward sea level + 8) and `landWeight` (relief ramps in over inland 0–0.35). Scale `coastPull` down and steepen the `landWeight` ramp (e.g. 0–0.03) by `cliffWeight`, so relief stays high to the waterline and drops into the sea within a few blocks.
3. Loosen the near-coast division of the 3D density amplitude by `cliffWeight` for overhangs and coves. Sea stacks can reuse the formation sampler on the ocean side, like the quartz spires.
4. The beach band becomes a "sea cliff" biome (bare stone or grass tops, no sand) where `cliffWeight` is high.

Watch: the seabed next to cliffs must also drop steeply, or cliffs stand on a shallow shelf. `inlandHeight` is already steep around inland 0; probe it first.

Roster:
- Brazil: varzea, igapo, cerrado, terra firme, lencois maranhenses, cloud forest, **pantanal**, **atlantic forest**
- California: redwood forest, **sequoia high sierra**, Joshua tree, seaside cliffs, chaparral, beaches
- Other: Torres del Paine (alpine lake), **patagonian steppe** (surrounds it), red desert (Uluru)

Trees: kapok, piuva, brazilwood, jacaranda, acai palm, rubber tree, mahogany.

## 5. Inland water (hardest, last)

- Today: water = `y <= seaLevel && !isInTerrain`, one global constant. No lakes possible.
- Free already: meshing, waves, camera-underwater, structure underwater rejection all level-agnostic.
- Broken: water side faces only emit against AIR — two water bodies at different heights leave hole at the lip. Must fix. Keep cave-air gate or lakes drain into caves.
- **Stage A — flooded biomes (varzea/igapo/pantanal) + ponds**: analytic per-column water table = f(XZ noise + biome), quantized to integer steps. Force terrain base height below table in floodable biomes (same trick as coast smoothstep). Chunk-local, no connectivity pass.
- **Stage B — alpine lakes**: don't detect basins, make them. Sparse cellular lake noise picks sites; depress terrain into bowl below lake level, raise rim. Water supported by construction.
- **Stage C — rivers**: ridged noise band carves channel. Robust version: channel down to sea level only (flat water, connects to ocean). True sloped rivers = stepped per-column water level, expect artifacts, prototype late.
- `heightfield` is scratch, discarded after structure placement — water passes needing it later must promote to member (like `biomes`) or recompute.

## Build order

1. Grass tint + biome GPU plumbing
2. Water sigmaA + fog color/density multiplier (same plumbing)
3. Trees + vines + decorator variety
4. Biome waves
5. Inland water A, B, C
