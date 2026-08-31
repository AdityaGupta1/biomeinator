_Last edited: 2026-08-30_

# glTF Loader

`src/scene/gltf_loader.cpp` imports a focused subset of glTF 2.0 for path tracing test
scenes. It resets and reinitializes `Scene`, uploads textures/materials, creates one
`Instance` per mesh primitive, and marks instances for BLAS build.

## Node Traversal

The loader currently processes mesh nodes directly and does not traverse parent/child node
hierarchy.
