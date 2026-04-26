_Last edited: 2026-04-26_

# Materials and Textures

Material and texture management in `src/scene/scene.h/cpp`.

## Materials

`Material` is a GPU-shared struct (defined in `common_structs.h`) with flags indicating which BxDF lobes are active (diffuse, glossy reflection, glossy transmission), plus base color, IOR, emissive color/strength, and optional texture IDs. Roughness is not yet implemented (there is a TODO for it). Materials are stored in a `MappedArray` that auto-resizes.

`Scene::addMaterial()` appends to the array and returns an index. Instances reference materials by index (`setMaterialIdx`). The terrain system pre-registers two materials (DEFAULT and WATER) at init; the glTF loader creates materials per-mesh.

## Textures

Textures are 2D RGBA8 sRGB with optional precomputed mip chains. Upload is deferred: `addTexture()` stashes raw pixel data in `pendingTextures`, and `uploadPendingTextures()` does the actual D3D12 texture creation + row-pitch-aligned copy on the next `Scene::update()`.

Each texture gets an SRV in the shared descriptor heap. The returned texture ID is the descriptor heap index, which shaders use for bindless access.

## Why Deferred Upload

Texture upload requires a command list (for `CopyTextureRegion`), but textures may be created during glTF loading which happens before the frame's command list recording. Deferring to `update()` ensures a valid command list context.

## Mip Handling

Two `addTexture` overloads exist: one accepting a full mip chain (`vector<vector<uint8_t>>`) and one accepting just mip 0. The glTF loader uses the single-mip overload (no mip generation). The terrain material system (`terrain_materials_helpers.h`) generates mip chains CPU-side and passes them via the multi-mip overload. Each mip level is uploaded as a separate `CopyTextureRegion` call into the appropriate subresource, with row pitch aligned to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`.
