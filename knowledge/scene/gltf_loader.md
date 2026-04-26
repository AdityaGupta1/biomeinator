_Last edited: 2026-04-25_

# glTF Loader

`src/scene/gltf_loader.cpp` imports a focused subset of glTF 2.0 for path tracing test
scenes. It resets and reinitializes `Scene`, uploads textures/materials, creates one
`Instance` per mesh primitive, and marks instances for BLAS build.

## Bounds

During POSITION import, each object-space vertex is transformed by the same node transform
assigned to the `Instance`, then fed into `Scene::expandBounds()`. These recorded
world-space bounds are used by NRC in glTF mode.

The loader currently processes mesh nodes directly and does not traverse parent/child node
hierarchy. Bounds intentionally follow that same behavior so NRC sees the same scene extent
that rendering uses.
