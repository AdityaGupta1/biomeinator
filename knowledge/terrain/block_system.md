_Last edited: 2026-09-08_

# Block System

`src/terrain/block.h/cpp` — block enum and per-block metadata looked up via `Blocks::getBlockData()`.

## JSON-Defined Blocks

Each block is a JSON file in `assets/blocks/` (type, shape, translucency, emission, texture names);
`Blocks::init()` parses them at runtime from the build's copied assets. The `Block` enum itself is
generated at CMake configure time (`block_ids.h.in` → `build/generated/block_ids.h`) from the JSON
**filenames only** — adding a block means adding a JSON file and reconfiguring, and a content edit
needs no recompile, just a build to re-run the asset copy. Ordering is air-first-then-alphabetical, so enum values are **not stable
across builds**; world exports stay valid because they carry a name palette (see
[world_export_import.md](world_export_import.md)). `AIR == 0` is the one fixed value — chunk block
storage assumes it, enforced by a `static_assert` in `block.h`.

Texture names in the JSONs refer to 16×16 PNGs in `assets/blocks/textures/` (shared freely
between blocks, e.g. `dirt` is also grass/snowy-grass bottom); `Blocks::init()` resolves them
to texture array slice indices, assigned in first-reference order — see
[greedy_meshing.md](greedy_meshing.md) for the ordering invariant and
[scene → materials_textures.md](../scene/materials_textures.md) for the `<name>.aux.png`
companions.

A block JSON that fails to open or parse logs an error and leaves that block's `BlockData` at
defaults (solid cube, no textures) rather than aborting — same spirit as the texture loader's
missing-file handling. Recognized custom-model definitions instead fail startup
on any field/model error, since falling back to an occluding cube would hide terrain.
Metadata is published only after the whole definition parses successfully.

## BlockType Drives Meshing

The non-obvious culling rules in `shouldGenerateFace`:
- **TRANSPARENT_CUTOUT** between two cutout blocks: only the one at the lower/equal position generates the face. This prevents double-rendering the shared boundary (both quads would be coplanar and z-fight).
- **WATER** only generates faces against AIR — water-water faces are hidden, and water against solid is hidden (the solid block's face covers it). Exception: `LIQUID_TOP` blocks always generate the +Y (top) face regardless of neighbor, so the water surface is always visible.

## GLASS blocks

`BlockType::GLASS` is a fully opaque-alpha cube that the path tracer shades as glass (see
[shaders → materials.md](../shaders/materials.md)). It is its own `BlockType` purely for the
culling rules: a face between two glass blocks would be a refraction interface *inside* what should
read as one solid crystal, and glass buried in rock is never seen, so both are culled — a crystal
formation meshes as a hollow shell. Solid neighbors are unaffected and still generate their face
towards glass, so rock and emitters behind a crystal stay visible through it.

Glass is opaque to the acceleration structure: its texels have alpha 1, so it needs no OMM or
anyhit handling, and shadow rays are blocked by it as they are by any rough transmissive surface.

## Procedural color

A block JSON's `proceduralColor` flag multiplies emission by a world-space ramp
while leaving diffuse and transmission texture colors unchanged, by setting `TRIANGLE_FLAG_PROCEDURAL_COLOR` at mesh time (CRYSTAL_CORE
uses it). The ramp itself lives in the shaders — see
[shaders → materials.md](../shaders/materials.md).

## BlockShape

`DECORATOR_CUSTOM` is an authored mesh that never hides adjacent solid/cutout cube
faces, regardless of the decorator's block type. `X_SHAPED` follows the same
neighbor rule. This lets mushrooms use `SOLID` without punching holes in their
ground or nearby leaves. Water retains its existing face rules. Segment occlusion
already requires both `SOLID` and `CUBE`.

Custom models are static GLBs in `assets/blocks/models`, with geometry cached during
`Blocks::init()` before worker threads start. They reuse one opaque 16px terrain
atlas named by the block, not glTF materials; see [custom_models.md](custom_models.md).

`X_SHAPED` blocks are rendered as two crossed diagonal quads (like Minecraft foliage). During mesh generation they also receive a random XZ jitter so adjacent grass blocks don't form a visible grid pattern.

`LIQUID_TOP` is a cube with the +Y face lowered by 1/8 block, creating the "not quite full block" water surface look.

## Emissive

`LAMP`, `LAVA`, and `LAVA_TOP` have `markAsEmitter = true`. Their triangles are tracked separately during mesh generation and fed to the path tracer's area light system. Marking an emissive block for explicit light sampling means setting this flag *and* authoring its texels in the assets: emission color lives in the block's diffuse texture (with zero diffuse implied) and per-texel strength in the red channel of its `<name>.aux.png` companion in `assets/blocks/textures/` — see [scene → materials_textures.md](../scene/materials_textures.md).

Emission on ray hits does not require `markAsEmitter`: the glowshroom model deliberately
uses an emissive cap mask with `markAsEmitter = false`, excluding its tiny triangles
from explicit area-light sampling. Both mushroom models disable diffuse transmission.

Cracked basalt crystal ore temporarily replaces 1% of generated cracked basalt, using
a world-seed/position hash independent of other generation RNG streams. It remains
emissive on ray hits but is not registered as an area light; the texture aux-R mask limits
emission to the user-authored ore pixels.
Imported worlds keep their saved blocks and do not reroll ore.
