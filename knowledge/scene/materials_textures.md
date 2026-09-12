_Last edited: 2026-09-10_

# Materials and Textures

Material and texture management in `src/scene/scene.h/cpp`.

## Materials

`Material` is a GPU-shared struct (defined in `common_structs.h`) with flags indicating which BxDF lobes are active (diffuse, glossy reflection, glossy transmission), plus base color, roughness (shared by both glossy lobes), IOR, emissive color/strength, and optional texture IDs. Materials are stored in a `MappedArray` that auto-resizes. The lobe semantics are described in [shaders → materials.md](../shaders/materials.md).

`Scene::addMaterial()` appends to the array and returns an index. Instances reference materials by index (`setMaterialIdx`). The terrain system pre-registers two materials (DEFAULT and WATER) at init; the glTF loader creates materials per-mesh.

## Textures

Textures are 2D RGBA8 with optional precomputed mip chains; `addTextureArray()` takes a format (sRGB default, plain UNORM for data textures like the terrain aux map, whose mips must be averaged without the sRGB transfer). Upload is deferred: `addTexture()` / `addTextureArray()` stash raw pixel data in `pendingTextures`, and `uploadPendingTextures()` does the actual D3D12 texture creation + row-pitch-aligned copy on the next `Scene::update()`.

Each texture gets an SRV in the shared descriptor heap. The returned texture ID is the descriptor heap index, which shaders use for bindless access.

`Material::normalTextureId` is a separate linear tangent-space normal texture for either
material path. `roughnessTextureId` holds glTF's linear metallic/roughness texture (G only),
multiplied by scalar roughness at hit resolution; packed-aux terrain continues to resolve
roughness from aux B. Both `addTexture` overloads accept an optional format, defaulting to
sRGB, so data maps do not undergo the sRGB transfer.

## Why Deferred Upload

Texture upload requires a command list (for `CopyTextureRegion`), but textures may be created during glTF loading which happens before the frame's command list recording. Deferring to `update()` ensures a valid command list context.

## Mip Handling

`PendingTexture` stores `sliceMipData[slice][mip]` + `arraySize`. Subresource index is computed via `D3D12CalcSubresource(mip, slice, ...)`. Row pitch is aligned to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` per row; each mip start in the upload buffer is aligned to `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`.

The glTF loader uses the single-mip overload (no mip generation). The terrain material system (`terrain_materials_helpers.h`) loads one 16×16 PNG per texture array slice from `assets/blocks/textures/` (slice order from `Blocks::getTextureNames()`) and generates each slice's mip chain CPU-side.

## Texture2D vs Texture2DArray

`uploadPendingTextures()` picks the SRV dimension from `arraySize`: 1 → `Texture2D`, >1 → `Texture2DArray`. Two invariants follow:

- **`addTextureArray()` asserts size > 1.** A single-slice texture must go through `addTexture()` so the SRV dim matches what the shader expects.
- **`MATERIAL_FLAG_ARRAY_TEXTURE` is per-material, not per-texture.** A material with this flag must have *both* `baseColorTextureId` and `auxTextureId` be array textures (or invalid). The shader (`sampleTexture` in `materials.hlsli`) uses one flag to branch the SRV cast for both. Mixing array+non-array on the same material miscasts the descriptor.

Terrain sets the flag (`setHasArrayTexture(true)`) on the DEFAULT material; glTF materials never do.
The same array/non-array invariant applies to the normal and separate roughness slots.

## Packed Aux (Terrain)

Terrain tangent-space normal maps are optional `<name>.normal.png` companions (16x16,
linear RGB, opaque alpha). They use a separate array aligned with the color/aux slices.
Missing slices contain flat +Z normals and are skipped using `TRIANGLE_FLAG_NORMAL_MAP`.
Mips average encoded vectors linearly; the shader normalizes after sampling and derives
the frame from triangle positions/UVs, without terrain tangent attributes. All surface ray
offsets use the geometric normal. Base normals are oriented
before applying the map; mapped normals are constrained to the geometric hemisphere and
are never flipped just because they face away from the viewing ray. Glossy surfaces retain
the additional reflection-normal correction.

Generate maps with `blender --background --python-exit-code 1 --python
blender/generate_normal_maps.py -- <block> --strength <value> --exponent <value>`.
The script resolves and deduplicates the block JSON's textures, then overwrites their
`.normal.png` companions. Shared textures affect every block using them. Strength
controls height range in texels (default 1, zero is flat); exponent controls the curve
`1-(1-h)^exponent` (default 1, larger values emphasize dark cracks). Optional `--invert`
makes bright areas recessed; `--face top|side|bottom` selects a single face's texture.
Edges wrap for tiling, with no heightfield blur. Current asset settings are:

- `stone --strength 1 --exponent 2`
- `marble --strength 1 --exponent 2`
- `basalt --strength 2 --exponent 2`
- `cracked_basalt --strength 2 --exponent 2`
- `clay --strength 0.5 --exponent 1`
- `white_crystal --strength 0.5 --exponent 1`

`auxTextureId` normally holds an emissive color texture; `MATERIAL_FLAG_PACKED_AUX` makes it a linear packed aux texture instead:
r = per-texel emissive strength, g = biome tint mask, b = roughness for faces shaded as glass
(read only there, so every other block's zero-filled b costs nothing). Aux data is authored as an optional
`<name>.aux.png` companion next to each block texture — most textures have none, and missing
files load as zero-filled slices. Emission *color* comes from the base
color texture — the shader zeroes diffuse wherever aux.r > 0, preserving the old
"emissive texels are pure emitters" behavior that `isPureEmitter` relies on. There is
no separate `emission.png` anymore.

Two invariants:
- Emission for a packed-aux material must be evaluated before anything clears its
  `baseColorTextureId`, because emission *color* lives in that texture. Both
  `trySplitMaterial`'s opaque branch and the per-bounce base-color bake in
  `path_tracing.rgs.hlsl` clear the ID, after which `getMaterialEmissiveColor` returns zero
  via its invalid-base-ID guard. The path tracer stays correct because `emissiveContrib` is
  computed at the top of the bounce loop before the split/bake, and `surfMaterial` is
  refetched from the hit buffer after each `TraceRay`.
- The aux texture must be loaded linear (`LoadTextureOptions::sRGB = false`) — mask and strength
  values would be distorted by the sRGB transfer during mip downsampling and sampling.
- The aux texture must inherit the diffuse texture's alpha channel
  (`LoadTextureOptions::alphaOverrides`) before its mips are built. `loadBlockTextureArray` decides
  premultiplied-alpha downsampling per tile from *that texture's own* alpha, and aux tiles are
  authored fully opaque (or absent, loading as zero-filled), so without the override a cutout
  tile's mask would be box-averaged against the
  texels the diffuse map cuts away. On an X-shaped block (~18% coverage) that drives the tint mask
  toward zero within one mip, and since tint-masked texels are authored grayscale the block reads
  gray at distance.
