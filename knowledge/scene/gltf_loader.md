_Last edited: 2026-09-04_

# glTF Loader

`src/scene/gltf_loader.cpp` imports a focused subset of glTF 2.0 for path tracing test
scenes. It resets and reinitializes `Scene`, uploads textures/materials, creates one
`Instance` per mesh primitive, and marks instances for BLAS build.

## Node Traversal

The loader currently processes mesh nodes directly and does not traverse parent/child node
hierarchy.

## Material Lobes

Lobe flags are derived from the PBR block plus the specular/transmission extensions, and
emission is independent of them: an emissive material keeps whatever lobes its parameters
describe, so "emission + diffuse" and "emission + specular" are valid engine materials. A pure
emitter is just an emissive material whose base color is black and whose `specularFactor` is 0,
which is what the Blender exporter writes for the Biomeinator BSDF group with Diffuse/Specular
off. Consequently every existing light in the test scenes still loads with no lobes.

Two non-obvious rules in the lobe derivation:

- `specularFactor == 0` disables glossy reflection only for dielectrics. Metals keep it, since
  for a specular-only material a zero factor just means no specular tint.
- A dielectric with a black base color and no base color texture gets no diffuse lobe, so with
  `specularFactor == 0` it has no lobes at all and renders black — the same as Cycles renders
  the group with Diffuse on and a black base color.
