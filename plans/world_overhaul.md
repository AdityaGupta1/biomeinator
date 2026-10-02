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
- **Facing (next PR, before vines, ferns and fallen logs):** structures can only write block IDs, and cube logs always stand upright. Plan: let structures write the existing sparse per-chunk block states (today only cave decorators set them), add an axis state for logs (mesher remaps end-grain faces and rotates bark UVs), and let custom-model blocks take a chosen face and turn instead of a random one. That one mechanism gives fallen logs, oriented fern fronds and vines; per-direction block IDs were the alternative, but only cover logs/vines and triple every log type.
- Vines: `BlockShape::WALL_MOUNTED` (one quad nudged 1/16 off the wall) or a surface-mounted custom model, oriented by the facing state above.
- Place vines from tree generators, NOT decorator (decorator stage neighbor reads = data race). `tryPlaceStructureBlock` skips obstacles without stopping — vine run needs own loop, break at first non-AIR.
- Same shape covers moss, lichen, trunk orchids later.

## 4. New biomes (data waves)

- Biome = enum entry + init block in `biome.cpp`. `uint8_t`, up to 255 fine.
- Crowding: lowland band gets full. Need more inland bands or extra selection axis (weirdness) — varzea/igapo/terra firme all hot+humid+flat.
- The noise chooses both the biome and the terrain, separately (see `knowledge/terrain/biome_system.md`). Don't add per-biome height offsets or swap profiles, and never have terrain read the label; a new landform is a bounded style on top of the shared height (like mesa terraces) driven by noise, with soft ramps for continuous styles such as roughness. Landform biomes go in the terrain regime table; others are climate targets.

### Highland and high-ground biomes

Each biome declares a `tier`; highland is chosen where mountain peak relief is substantial (`isHighland`), and the highland tier holds only relief biomes: today just mountains. Savanna and ice fields are climate zones and live in the lowland tier. So the new mountain-country biomes go in the **highland** tier, each a climate target within relief ground:

| climate | biome | character |
|---|---|---|
| warm, very humid | cloud forest | mossy twisted trees, vines, dense undergrowth, heavy fog (fog density plumbing in §2) |
| cold, humid | highland taiga | firs and boreal pines on slopes below the snow line (lowland taiga exists) |
| cool, moderate | highland moor / alpine meadow | grass, heather, flowers, boulders, sparse conifers |
| dry (hot or cold) | high desert steppe | sagebrush, junipers, sparse grass; dry relief ground mesa/red desert don't claim |
| cold, dry / very high | mountains (existing) | bare stone peaks |

Redwood forest is the exception: mild, very humid, **lowland** (done in the lowland wave below). Favoring ground near the coast is still open.

Cloud forest shares Tianzi's warm/humid climate; they separate by erosion (Tianzi takes preserved relief as a regime, cloud forest the eroded rolling highlands left over).

Climate matching within a tier is 2D (temperature, humidity); peak no longer takes part, since relief already picks the tier. That removed the old issue where high-peak lowland picked whichever low-peak target was least wrong (forest on hot, dry ground).

### Lowland wave (branch `more_lowland_biomes`, 2026-10-02)

Done:
- **Biomes:** flower meadow, old-growth forest, cherry grove, taiga, birch forest, redwood forest
  (all lowland climate targets).
- **Trees:** fir (vanilla spruce layer pattern), cherry, boreal pine, boreal and autumn birch (tall
  variant), giant oak, redwood (tall narrow cone with buttress roots); the pine is reworked into
  branch pads and the large oak into straight limbs ending in leaf clumps. Design rule that came out
  of iterating: clean, mostly symmetric layers (no full-square leaf layers, no drooping); ragged
  layers only for the redwood, and never leaves that float.
- **Ground and decor:** Yuushya flowers, tall plants and podzol/coarse dirt; two-tall decorators
  (`upperHalf`); single-species flower drifts; noise-driven top-block patches (podzol, coarse dirt);
  `StructureGen::emptyWeight` for thinning a mixed grid without widening it.
- **Placement:** land columns match climate targets on the climate of a weighted Voronoi **climate
  cell** (~384 blocks, warped, ragged edges, ±0.1 per-cell climate offset) instead of their own,
  which removed slivers and in-between-biome ribbons; climate noise scale doubled. See
  `knowledge/terrain/biome_system.md`.
- **Tooling:** `BiomeScanner --coverage` (land share and patch sizes over many seeds, `--cells=0`
  to compare) and a cell debug view.

Coverage after the wave (8 seeds): lowland climate biomes range from about 8% (taiga, plains) down
to about 3.5% (forest, savanna); forest is no longer dominant. Mountains is 5.2%, Tianzi 7%,
red desert 5.9%, mesa 3.2%.

Tried and removed: per-biome climate biases calibrated by the scanner to hit target shares. It hit
the shares exactly but pushed biomes far from their targets (forest and birch forest onto ground
cold enough for snow layers, which follow column temperature, not labels). Revisit share balancing
after the terrain shape work, preferring target placement over large biases.

### Next for biomes

- **Highland wave.** Highland (`isHighland`, threshold 0.1 on `mountainPeakWeight`) is only ~5% of
  land, all mountains; split among the planned highland biomes each would be ~1%. Lower the
  threshold (e.g. 0.03-0.05) so foothills count, give bare mountains the coldest/highest climate
  target, and add the highland biomes above.
- **Rainforest** (warm, very humid lowland; mahogany emergents over dense undergrowth) needs a
  mahogany tree and benefits from vines. Warm humid ground currently goes to cherry grove and
  redwood forest.
- **After facing:** giant ferns and fallen logs (redwood/old-growth floors), vines, then leaf litter
  and petal carpets (flat custom-model quads, as in vanilla).
- **Unplaced blocks:** peony and rose bush are registered but no biome places them yet (flower
  meadow fits), and nothing places cattails, so `CATTAIL_BOTTOM`'s `upperHalf` is unused.
- **Snow on labels:** cold-climate snow cover starts at temperature -0.25; keep cool forest targets
  (birch forest at -0.3) in mind when moving targets, or they read as snowy forests.

### Seaside cliffs (Big Sur)

A coastal landform, with the label derived from it:

1. `cliffWeight` from high peak + low erosion near the coast.
2. Let relief reach the shore: two terms flatten every coast today — `coastPull` (toward sea level + 8) and `landWeight` (relief ramps in over inland 0–0.35). Scale `coastPull` down and steepen the `landWeight` ramp (e.g. 0–0.03) by `cliffWeight`, so relief stays high to the waterline and drops into the sea within a few blocks.
3. Loosen the near-coast division of the 3D density amplitude by `cliffWeight` for overhangs and coves. Sea stacks can reuse the formation sampler on the ocean side, like the quartz spires.
4. The beach band becomes a "sea cliff" biome (bare stone or grass tops, no sand) where `cliffWeight` is high.
5. Cliff tops grow wind-sculpted **Monterey cypress** (flat, windswept crowns leaning inland), which likely needs a darker, denser leaf block than the swamp's bald cypress.

Watch: the seabed next to cliffs must also drop steeply, or cliffs stand on a shallow shelf. `inlandHeight` is already steep around inland 0; probe it first.

Also: ocean and beach tiers use fixed inland cutoffs (-0.15, 0) in `getClosestBiome`. Once cliffs change coastal shaping, derive the beach/cliff label from the same coast factor that shapes the terrain, or beach labels will land on cliffs.

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
