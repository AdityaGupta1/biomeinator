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
- `BiomeData` too thin for terrain shaping: add per-biome base-height offset, surface multiplier, ridge params. Blend neighbor columns' params to avoid border cliffs.

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
