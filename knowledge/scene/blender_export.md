_Last edited: 2026-09-02_

# Blender glTF Export

`blender/biomeinator_bsdf.py` is an addon defining the Biomeinator BSDF node group and the
glTF export operator; `blender/reexport_gltf.py` drives it headlessly
(`blender --background <file>.blend --python reexport_gltf.py -- --gltf <out>.gltf`) so test
scenes can be regenerated without opening the UI.

Blender is not on `PATH`; it is typically reachable through the shortcut `D:\blender.lnk`. If
that shortcut is missing, ask the user where Blender is installed rather than guessing.

## Node group design

The group mixes two Fresnel conventions on purpose. Specular reflection over diffuse uses the
Fresnel node (macro normal), which is what the engine's `walterFresnel` lobe selection does.
Specular reflection + transmission is one OSL `dielectric_bsdf` closure with per-microfacet
Fresnel and `multi_ggx` energy compensation — the Glass BSDF node would do the same but tints
both lobes with one colour, whereas the engine (and `glass_diamonds`) tint reflection and
transmission separately. Transmission replaces diffuse entirely, matching the engine invariant
that a transmissive material has no diffuse lobe. Transmission without Specular must keep
Roughness at 0: the engine only supports rough transmission inside the dielectric closure
(asserted in `Scene::addMaterial`), so the Refraction node's roughness input exists only for
socket parity and exporting it non-zero fails at load.

`dielectric_bsdf` gotchas, both handled inside the embedded OSL text: it takes alpha
(roughness²), and it does not flip the IOR on backfaces (Cycles' own glass node shader does
`backfacing() ? 1/IOR : IOR` before calling it).

The Script node means a scene rendering transmissive materials must enable Cycles' Open
Shading Language option (`scene.cycles.shading_system`); with it off the node is simply
dropped, which is harmless for materials whose mix factor never selects it.

Each .blend carries its own copy of the group. `ensure_node_group` upgrades a stale copy in
place (adds missing sockets, then rebuilds the internal nodes when the dielectric node is
absent), preserving material-level socket values and links. Old test scenes do not need
upgrading: the rewiring is render-identical for every lobe combination they use, and only a
scene with rough transmission needs the new closure.

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

Because the URI is derived from the .blend's image path, all test textures live in
`tests/gltf/_textures/` and every .blend references them there (`//../_textures/<name>`), so
the glTFs point at one copy (`../_textures/<name>`) and nothing is duplicated per scene.

See [gltf_loader.md](gltf_loader.md) for the import side.
