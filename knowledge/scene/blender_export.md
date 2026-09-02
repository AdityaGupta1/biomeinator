_Last edited: 2026-09-01_

# Blender glTF Export

`blender/biomeinator_bsdf.py` is an addon defining the Biomeinator BSDF node group and the
glTF export operator; `blender/reexport_gltf.py` drives it headlessly
(`blender --background <file>.blend --python reexport_gltf.py -- --gltf <out>.gltf`) so test
scenes can be regenerated without opening the UI.

Blender is not on `PATH`; it is typically reachable through the shortcut `D:\blender.lnk`. If
that shortcut is missing, ask the user where Blender is installed rather than guessing.

## Why materials are proxied on export

The engine reads a plain glTF PBR material, so `export_gltf` temporarily swaps each
Biomeinator BSDF group for a Principled BSDF wired to the fields the exporter writes, exports,
then restores the original nodes. Anything the group exposes that has no Principled equivalent
is lost, so the group's semantics and the proxy mapping must be kept in sync.

The exporter also omits factors equal to the glTF spec default, which would leave a material
silently depending on the loader's defaults, so `_make_material_factors_explicit` writes
`metallicFactor`/`roughnessFactor` back into the JSON after export.

## Textures: keep originals

`export_keep_originals=True` is passed so the exporter emits a relative URI to the image file
the .blend already references. Without it the exporter copies every referenced image into the
export directory, which silently duplicates a texture shared between test scenes (e.g. two
tests using the same `shoebill.png`). The flag's caveat is that Blender no longer repacks
multiple images into the single ORM texture glTF expects; scenes using separate
metallic/roughness/occlusion maps would export incorrectly.

Because the URI is derived from the .blend's image path, a texture shared across tests must be
referenced in Blender at its real location (e.g. `//../emissive_texture/shoebill.png`), not
copied next to the .blend.

See [gltf_loader.md](gltf_loader.md) for the import side.
