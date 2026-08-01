_Last edited: 2026-08-01_

# Materials and Textures

Material and texture management in `src/scene/scene.h/cpp`.

## Materials

`Material` is a GPU-shared struct (defined in `common_structs.h`) with flags indicating which BxDF lobes are active (diffuse, glossy reflection, glossy transmission), plus base color, IOR, emissive color/strength, and optional texture IDs. Roughness is not yet implemented (there is a TODO for it). Materials are stored in a `MappedArray` that auto-resizes.

`Scene::addMaterial()` appends to the array and returns an index. Instances reference materials by index (`setMaterialIdx`). The terrain system pre-registers two materials (DEFAULT and WATER) at init; the glTF loader creates materials per-mesh.

## Textures

Textures are 2D RGBA8 with optional precomputed mip chains; `addTextureArray()` takes a format (sRGB default, plain UNORM for data textures like the terrain aux map, whose mips must be averaged without the sRGB transfer). Upload is deferred: `addTexture()` / `addTextureArray()` stash raw pixel data in `pendingTextures`, and `uploadPendingTextures()` does the actual D3D12 texture creation + row-pitch-aligned copy on the next `Scene::update()`.

Each texture gets an SRV in the shared descriptor heap. The returned texture ID is the descriptor heap index, which shaders use for bindless access.

## Why Deferred Upload

Texture upload requires a command list (for `CopyTextureRegion`), but textures may be created during glTF loading which happens before the frame's command list recording. Deferring to `update()` ensures a valid command list context.

## Mip Handling

`PendingTexture` stores `sliceMipData[slice][mip]` + `arraySize`. Subresource index is computed via `D3D12CalcSubresource(mip, slice, ...)`. Row pitch is aligned to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` per row; each mip start in the upload buffer is aligned to `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`.

The glTF loader uses the single-mip overload (no mip generation). The terrain material system (`terrain_materials_helpers.h`) generates mip chains CPU-side then splits them into per-tile slices.

## Texture2D vs Texture2DArray

`uploadPendingTextures()` picks the SRV dimension from `arraySize`: 1 → `Texture2D`, >1 → `Texture2DArray`. Two invariants follow:

- **`addTextureArray()` asserts size > 1.** A single-slice texture must go through `addTexture()` so the SRV dim matches what the shader expects.
- **`MATERIAL_FLAG_ARRAY_TEXTURE` is per-material, not per-texture.** A material with this flag must have *both* `baseColorTextureId` and `auxTextureId` be array textures (or invalid). The shader (`sampleTexture` in `materials.hlsli`) uses one flag to branch the SRV cast for both. Mixing array+non-array on the same material miscasts the descriptor.

Terrain sets the flag (`setHasArrayTexture(true)`) on the DEFAULT material; glTF materials never do.

## Packed Aux (Terrain)

`auxTextureId` normally holds an emissive color texture; `MATERIAL_FLAG_PACKED_AUX` makes it a linear packed aux texture instead:
r = per-texel emissive strength, g = biome tint mask. Emission *color* comes from the base
color texture — the shader zeroes diffuse wherever aux.r > 0, preserving the old
"emissive texels are pure emitters" behavior that `isPureEmitter` and NRC rely on. There is
no separate `emission.png` anymore.

Two invariants:
- Emission for a packed-aux material must be evaluated before anything clears its
  `baseColorTextureId`, because emission *color* lives in that texture. Both
  `trySplitMaterial`'s opaque branch and the per-bounce base-color bake in
  `path_tracing.rgs.hlsl` clear the ID, after which `getMaterialEmissiveColor` returns zero
  via its invalid-base-ID guard. The path tracer stays correct because `emissiveContrib` is
  computed at the top of the bounce loop before the split/bake, and `surfMaterial` is
  refetched from the hit buffer after each `TraceRay`.
- The aux texture must be loaded linear (`loadTexture(..., sRGB=false)`) — mask and strength
  values would be distorted by the sRGB transfer during mip downsampling and sampling.
