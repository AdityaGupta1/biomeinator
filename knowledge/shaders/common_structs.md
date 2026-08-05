_Last edited: 2026-08-04_

# Common CPU/GPU Structs

`src/rendering/common/` — headers shared verbatim between C++ and HLSL. This is the primary mechanism for passing data from the CPU to shaders. See [settings → settings_manager.md](../../../settings/settings_manager.md) for how runtime settings feed into these params, and [rendering → param_blocks.md](../../../rendering/param_blocks.md) for how they are uploaded each frame.

The headers use preprocessor macros to alias HLSL types to DirectX math types when compiled as C++:
```cpp
#define float3 DirectX::XMFLOAT3
#define uint uint32_t
// etc.
```
If a new `#define` macro of this kind is added, the corresponding `#undef` macro must be added at the end of the file.

All structs must be 16-byte aligned (pad manually with `uint padN`). Padding members are named `pad0`, `pad1`, etc. and must always be renumbered from 0 — if a pad is replaced by a real field, the remaining pads must be renamed to keep them zero-indexed.

---

## common_structs.h — Geometry and Material Data

POD structs that need to be accessible on both CPU and GPU. These live in GPU buffers and are indexed at ray hit time.

**`InstanceData`** — per-instance GPU record: offsets into the shared vertex/index/per-tri/area-light buffers, a `transformOffset` (integer world-space offset to avoid float precision loss), and a `materialIdx`.

**`Material`** — PBR material with a `flags` bitfield (diffuse / glossy reflection / glossy transmission). Helper methods (`hasDiffuse()`, `isDelta()`, `canScatter()`, etc.) are compiled only in C++ via `#ifdef __cplusplus`.

**`AreaLight`** — triangle light source. Positions do not include `transformOffset` or `globalInstanceOffset` — shaders must apply those offsets.

**`Vertex`** — normal and uv are packed into one `uint` each (octahedron snorm16 and f16 pair). The CPU encoders in `util/packing.h` must stay bit-identical to the decoders in `shaders/util/packing.hlsli`. Only `pos_OS` is unpacked, which is what lets BLAS builds (pos at offset 0, `sizeof(Vertex)` stride) and the water displacement pass work without decoding. Cube-face normals (±X/±Y/±Z) encode exactly; arbitrary normals quantize (~0.004° max error), which near-bit-exact golden tests are sensitive to.

The remaining structs (`HitInfo`, `GbufferData`, `PerTriangleData`) are self-explanatory from the source.

---

## common_params.h — Per-Frame Constant Buffers

Structs that are uploaded to the GPU each frame (or once at init) via `ParamBlockManager`, forming the global constant buffer at `b0, space0`.

**`ConstantParams`** — `rngSeed`, set once at startup from a random device in `initConstantParams()`, not re-randomized each frame.

**`CameraParams`** — current and previous frame camera state, including `globalInstanceOffset` / `prevGlobalInstanceOffset` (the integer world-space chunk offset used to reconstruct true world positions in shaders).

**`SceneParams`** — scene-level flags: `voxelMode`, `numAreaLights`, `cameraUnderwater`, voxel world bounds.

**`RenderParams`**, **`RadianceCacheParams`**, **`DebugParams`** — mirrors of the corresponding settings groups. Field names match the setting names closely enough to cross-reference directly.

**`HeapIndices`** — bindless descriptor heap indices for all render targets, populated each frame as targets are allocated.

---

## common_registers.h — Register / Space Assignments

Centralises all shader register slots so that no literal register numbers appear in shader source. The `REGISTER_U/T/B/S(PREFIX, PARAM)` macros expand to a full `register(tN, spaceM)` declaration by combining the prefix's `_REGISTER_SPACE` and the slot's `_REGISTER_` constant.

The `NV_SHADER_EXTN_SLOT u1738, space1738` is the NVAPI/SER extension hook — the magic number 1738 is arbitrary but must not conflict with real slots.

---

## common_enums.h — Shared Enumerations

Enums used on both CPU (settings validation, ImGui combos) and GPU (shader branches). Each has a `COUNT` sentinel used for bounds checking on the CPU side.

---

## common_hitgroups.h — Hit Group Indices

Integer indices into the DXR shader table, one per pass (G-buffer, path tracing) and hit group type (primary geometry, lights, dome light).

---

## common_settings.h — Compile-Time Constants

For values that need to match between CPU dispatch calls and GPU shader code but are fixed at compile time (workgroup sizes, buffer sizes). Not for runtime-configurable values — those go through `common_params.h`.
