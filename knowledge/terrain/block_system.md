_Last edited: 2026-07-29_

# Block System

`src/terrain/block.h/cpp` — block enum and per-block metadata looked up via `Blocks::getBlockData()`.

## BlockType Drives Meshing

The non-obvious culling rules in `shouldGenerateFace`:
- **TRANSPARENT_CUTOUT** between two cutout blocks: only the one at the lower/equal position generates the face. This prevents double-rendering the shared boundary (both quads would be coplanar and z-fight).
- **WATER** only generates faces against AIR — water-water faces are hidden, and water against solid is hidden (the solid block's face covers it). Exception: `LIQUID_TOP` blocks always generate the +Y (top) face regardless of neighbor, so the water surface is always visible.

## BlockShape

`X_SHAPED` blocks are rendered as two crossed diagonal quads (like Minecraft foliage). During mesh generation they also receive a random XZ jitter so adjacent grass blocks don't form a visible grid pattern.

`LIQUID_TOP` is a cube with the +Y face lowered by 1/8 block, creating the "not quite full block" water surface look.

## Emissive

`LAMP`, `LAVA`, and `LAVA_TOP` have `emitsLight = true`. Their triangles are tracked separately during mesh generation and fed to the path tracer's area light system. Adding an emissive block means setting this flag *and* authoring its texels in the assets: emission color lives in `diffuse.png` (with zero diffuse implied) and per-texel strength in `aux_map.png`'s red channel — see [scene → materials_textures.md](../scene/materials_textures.md).
