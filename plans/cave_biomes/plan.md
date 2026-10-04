# Mineral Cave Biomes

Goal: new cave biomes themed on real mineral deposits, inspired by the AMNH Mignone Halls of
Gems and Minerals (visited 2026-09-28). Each biome is grounded in a real geological setting,
with deliberate creative liberty where it looks better (colour-theory splits, oversized
crystals, glowshroom-tier accents). Designed 2026-09-28/29 over a reference-board review and
CPU prototypes; nothing is implemented in the engine yet.

## Scope

First batch:

| Biome | Real basis | Look |
|---|---|---|
| **Contact Marble** | Mogok (ruby/spinel), Sar-e-Sang (lapis) | White marble; one of two palettes per region (below) |
| **Pegmatite** | Tourmaline gem pockets (Minas Gerais, Pala, Maine) | Desaturated granite, watermelon elbaite column bundles |
| **Opal Fields** | Queensland boulder opal, Coober Pedy, Mintabie | Pink/cream claystone strata with horizontal opal seams |
| **Oxidized Copper** | Supergene zones (Bisbee, Tsumeb, Kapunda, Boleo) | Desaturated banded strata, blue/teal crusts, rare crystals |

Deferred as later deep biomes: Hydrothermal Vug (fluorite, celestine), Sulfur/Hellish
(sulfur, realgar/orpiment, blue sulfur fire, lava), Kyanite Schist (giant kyanite blades,
garnets in mica schist). Dropped: Franklin NJ, amazonite, lepidolite, smoky quartz, cuprite
(red clashes), opalized fossils (no creatures to leave them), meteorites/pallasites,
amethyst geodes. The existing CRYSTALS biome stays as-is for now.

## Per-biome direction (from the board review)

**Contact Marble**
- One biome with a **palette chooser**, not two biomes: red and blue minerals together looked
  discordant, so a low-frequency field picks a palette per region.
  - *Ruby palette*: white marble, graphite streaks (dark band in white host), ruby/spinel.
  - *Lapis palette*: grey-white marble, lapis veins/lenses with pyrite specks, metallic pyrite
    cubes (the gold-with-ultramarine look from the AMNH lapis and the Sar-e-Sang specimen).
- Crystals are **voxel** shapes for now, not custom models: an L1 ball (|x|+|y|+|z| ≤ r) is an
  octahedron and reads well at r = 1-3.
- **Marble material fix first**: the current texture's normal map is too strong; it wants rough
  glossy reflection and little or no normal map. Also benefits LUSH, which uses marble as its
  secondary rock. Contact Marble sits next to LUSH in climate space so overgrown marble blends in.

**Pegmatite**
- Watermelon elbaite only (it already carries two colours). Green/pink only; the thin white
  band between them in real cross-sections is too thin to voxelize.
- Mostly desaturated host stone with colourful crystals; big feldspar grains only as a rare
  accent (the Ruggles K-feldspar colour is too saturated as a base).
- Showpiece: Tarugo-style bundles of parallel columns with **terraced tops**, rounded-triangle
  cross-sections (tourmaline is trigonal), zoning along the axis with random pink-tip or
  green-tip per bundle (both occur in nature). Scaled-up sprays jutting from walls.
- Pocket clay as a rusty patch around cluster bases (too small to do in the grooves).

**Opal Fields**
- Pinkish/cream claystone strata with **horizontal seams** ("levels" follow sedimentary
  layers, as in the Mintabie wall photo). Full opal blocks → opal "ore" (flecks) fringe →
  plain host.
- Fire opal as a rare variant of the blue-green opal, chosen per region.
- Shallow biome: sun shafts + glowworms, but see the lighting finding below — it needs a
  neutral main light too.

**Oxidized Copper**
- Desaturated banded strata (the Kapunda wall: ochre/pink/grey-green) as background so the
  crystals stand out. Avoid dark host rock (the dark-basalt lighting problem CRYSTALS has).
- Blue-green crusts (azurite, malachite, caledonite) via the existing skin/patch system;
  malachite/azurite along joints.
- Rare decorations: indigo boleite cubes (the best colour reference overall), pastel
  aurichalcite, glassy scorodite, rare white/pale-blue Bisbee calcite-aragonite formations.
- Boxwork holes (the Singing Stone) as a possible fine carve detail.
- Green water in some pools, **after** the per-biome water colour plan
  ([../biome_water_color.md](../biome_water_color.md)) lands.

## Lighting rules

1. **Primary lights must be NEE-able**, as every existing cave biome has one (lamp or rainbow
   crystal core). That means exposed `markAsEmitter` geometry whose surface is mostly
   emissive. Custom models are fine (their triangles are collected too).
2. **An emitter can't also be absorptive glass or metal.** Glass occludes shadow rays, so an
   emitter inside crystal is only reachable by BSDF paths. Use the crystal-cluster pattern
   (knowledge/terrain/cave_structure_system.md): a bare emitter core is the light; glass or
   metal minerals around it are dressing that tints or reflects it.
3. **Emitters should be near-white.** The prototypes showed any single saturated light floods
   the whole cave with its colour (ruby, tourmaline cores, glowworms and uranium all did);
   the main light's colour is effectively the biome's white balance. Colour comes from the
   glass the light passes through and from the host rock. Saturated emitters stay small accents.
4. **Texture-mask glow is accent-only.** The light tree's flux estimate ignores textures, and
   the user dislikes the current glowing-ore look in CRYSTALS.
5. **Accents with creative liberty are welcome** (glowshroom tier): glowworms (approved; also
   planned for future underground rivers), cave pearls (maybe).

| Biome | Main light | Dressing / accents |
|---|---|---|
| Contact Marble (ruby) | warm near-white core | ruby glass octahedra |
| Contact Marble (lapis) | warm gold-white core | metallic pyrite cubes |
| Pegmatite | near-white core at bundle base | elbaite glass columns |
| Opal | **open question** (warm lamps worked in the prototype) + sun shafts | glowworm beads/threads |
| Copper | uranium (cuprosklodowskite) crust patches, pale green-white emission | neon crust texture |
| Sulfur (later) | animated blue sulfur fire (soul-fire style, gentle procedural flicker on `animTime`) + lava | — |

## Terrain architecture

**Shared cave fields.** As on the surface (knowledge/terrain/terrain_profiles.md: noise picks
the biome and the terrain separately), cave geometry comes from continuous shared fields —
vein/seam frequency and thickness, strata banding, carve roughness, terrace amount, palette
selector — sampled on the existing coarse cave grid. Biomes only choose which blocks fill that
geometry, so borders change material without cutting veins off, and biome selection reads the
same fields (strong strata and terraces tend toward Opal/Copper, like dry climate tends to Mesa).

**Terraces**: partial quantization of the carve threshold in y, bounded to about one step and
faded in by field — the Mesa lesson; a full remap produced huge concentric terraces.

**Palette struct** replacing today's loose `secondaryBaseBlock` / `secondarySkinFringeBlock` /
`secondaryFlatSurfaceBlock`: {base, skin fringe, flat surface, crystal, emitter} with up to two
per biome, chosen by the existing low-frequency rock field. LUSH and CRYSTALS migrate to it.
Structures read the field at their anchor during the column scan (so a cluster stays one
palette across chunk borders); decorators can't (the field is scratch, released before
decoration), so each palette gets its own base block and decorators filter on it, like FERN.

**Vein/seam helper** shared by lapis lenses and opal seams: full mineral → ore fringe → host.

**Cave rework** (needed before the shallow biomes):
- Sealed by default: the surface fade becomes a guarantee (no cave air within N blocks of the
  surface).
- Entrances carved deliberately and analytically in the fill loop: sun shafts (radius 2-4,
  wobbly, near-vertical) and wider sloped entrances on a seeded jittered grid, each testing the
  cave noise along its own axis to decide whether it reaches a cave. Pure function of seed and
  position, so no cross-chunk reads or seams. Skip under water/lakes and formations.
- A depth axis in cave biome selection: shallow weathering zones (Opal, Copper), mid (Marble,
  Vug), deep (Pegmatite, Schist), deepest (Sulfur).
- Test impact: voxel goldens load imported worlds and are unaffected; only the procedural
  `grass_biome_blend` could change, and only if an opening appears in its view.

## Rendering prerequisites

- **Marble material**: rough glossy reflection, weaker normal map.
- **Metallic lobe** following Cycles (Principled F82-tint conductor), applied per triangle like
  the glass override (metal face flag, tint/roughness from the aux texture). Needed for pyrite.
- **Volume absorption in glass** (Beer-Lambert inside the medium). Today glass tint is
  interface-only; water's segment absorption is a starting point. Needed for the gem look
  (the pink corundum and scorodite "absorption refs").
- Later: animated emission for blue fire; biome water colour for Copper's green pools.

## Prototype findings

See [prototype/](prototype/) and [prototype/results/](prototype/results/).
- Voxel octahedra, terraced trigonal column bundles, pyrite cubes and horizontal seams all read
  well at voxel scale.
- Rule 3 above came from here: saturated emitters had to be desaturated in every biome.
- Glowworms alone light Opal cyan; warm wall lamps + glowworm accents look right
  (results/3_opal.png, top vs middle).
- Shared fields keep veins/strata continuous across biome borders (results/5, 6).

## Suggested order

1. Marble material fix.
2. Shared voxel crystal generator: terraced columns, L1 octahedra, cubes, jutting blades, with
   an emitter-core hook.
3. Palette struct refactor (LUSH/CRYSTALS migrate), then Contact Marble (ruby first; lapis once
   the metallic lobe lands).
4. Pegmatite.
5. Cave rework (seal, shafts/entrances, depth axis) + shared fields (veins, strata, terraces).
6. Opal and Copper (share strata code).
7. Deep biomes: Vug, Schist, Sulfur.

## Open questions

- Opal's main light: warm lamps (mining look, reuses LAMP) or a pale opal-themed emitter?
- Cave pearls: keep or drop?
- Lapis before or after the metallic lobe?
