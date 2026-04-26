_Last edited: 2026-04-25_

# Scene

`src/scene/scene.h/cpp` owns ray-traceable instances, shared geometry buffers, materials,
textures, TLAS build state, and scene-level metadata needed by rendering systems.

## Bounds

`Scene` tracks optional world-space bounds for loaded glTF geometry. The glTF loader expands
these bounds from transformed vertex positions as it creates instances. The renderer uses
them for NRC `sceneBoundsMin/Max` in glTF mode; voxel mode still uses terrain bounds
instead.

The bounds intentionally match the current glTF loader's transform behavior. If loader
support for parent/child transform hierarchy is added later, bounds expansion should move
with that final world-transform calculation.
