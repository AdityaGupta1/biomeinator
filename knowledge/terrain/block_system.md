_Last edited: 2026-08-23_

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
[scene → materials_textures.md](../scene/materials_textures.md) for the `<name>_aux.png`
companions.

A block JSON that fails to open or parse logs an error and leaves that block's `BlockData` at
defaults (solid cube, no textures) rather than aborting — same spirit as the texture loader's
missing-file handling.

## BlockType Drives Meshing

The non-obvious culling rules in `shouldGenerateFace`:
- **TRANSPARENT_CUTOUT** between two cutout blocks: only the one at the lower/equal position generates the face. This prevents double-rendering the shared boundary (both quads would be coplanar and z-fight).
- **WATER** only generates faces against AIR — water-water faces are hidden, and water against solid is hidden (the solid block's face covers it). Exception: `LIQUID_TOP` blocks always generate the +Y (top) face regardless of neighbor, so the water surface is always visible.

## BlockShape

`X_SHAPED` blocks are rendered as two crossed diagonal quads (like Minecraft foliage). During mesh generation they also receive a random XZ jitter so adjacent grass blocks don't form a visible grid pattern.

`LIQUID_TOP` is a cube with the +Y face lowered by 1/8 block, creating the "not quite full block" water surface look.

## Emissive

`LAMP`, `LAVA`, and `LAVA_TOP` have `emitsLight = true`. Their triangles are tracked separately during mesh generation and fed to the path tracer's area light system. Adding an emissive block means setting this flag *and* authoring its texels in the assets: emission color lives in the block's diffuse texture (with zero diffuse implied) and per-texel strength in the red channel of its `<name>_aux.png` companion in `assets/blocks/textures/` — see [scene → materials_textures.md](../scene/materials_textures.md).
