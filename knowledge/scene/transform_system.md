_Last edited: 2026-04-26_

# Transform System

Instances use a split transform to avoid floating-point precision loss far from the origin.

## The Problem

A float transform with absolute world-space translation loses sub-millimeter precision beyond ~1000 units. In a voxel world that extends thousands of blocks, this causes visible jitter and seam artifacts.

## The Solution: Three Layers

1. **`Instance::transform`** (XMFLOAT3X4) — a float matrix for rotation/scale/sub-block positioning. For terrain chunks this is identity; for glTF instances it carries the model transform. Translation in this matrix is relative to the instance's origin, not world origin.

2. **`Instance::transformOffset`** (ivec3) — integer world-space offset of the instance's origin. For terrain chunks this is the chunk's block-space corner position. Being integer, it's exact regardless of distance from origin.

3. **`Scene::globalInstanceOffset`** (ivec3) — an integer offset subtracted from all instances when building the TLAS. Set to the camera's XZ position each frame (Y=0). This re-centers the entire scene near the origin so the float translations in the TLAS instance descriptors stay small.

## How They Combine

In `makeTlas`:
```
TLAS transform[i][3] += (instance.transformOffset - globalInstanceOffset)
```

The camera position is also expressed relative to `globalInstanceOffset`, so the camera's float-space position stays near zero.

## Why `prevGlobalInstanceOffset` Exists

When `globalInstanceOffset` changes between frames, the previous frame's view matrix needs correction for motion vectors. The camera applies a translation of `(globalInstanceOffset - prevGlobalInstanceOffset)` to `worldToPrevView` to compensate.

## Shader Side

Shaders receive `globalInstanceOffset` and per-instance `transformOffset` via constant buffers. When computing true world-space positions (e.g. for noise sampling or light positions), they add these integer offsets back.
