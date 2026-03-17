# Radiance Cache Implementation - Task Tracking

## Status Overview

| Step | Description | Status |
|------|-------------|--------|
| 1 | Add RC Constants, Params, and Settings | Done |
| 2 | Create GPU Buffers | Done |
| 3 | Eviction/Clear Compute Shader + Pipeline | Done |
| 4 | Resolve Compute Shader + Pipeline | Done |
| 5 | Radiance Cache Utility Header (HLSL) | Done |
| 6 | RC Update Pass (Cache Training) | Done |
| 7 | Debug Visualization | Done |
| 8 | Cache Read in Main Render Pass | Not Started |
| 9 | Cascading Voxel Sizes | Done |
| 10 | Offset-Aware Grid Invalidation Fix | Done |
| 11 | Tuning and Polish | Not Started |
| 12 | Dedicated Debug View Pass + Unified Debug View Dropdown | Done |

## Step 1: Add RC Constants, Params, and Settings

- [x] Add constants to `common_settings.h`
- [x] Add `RadianceCacheParams` struct to `common_params.h`
- [x] Add `rcParams` to `global_params.hlsli` cbuffer
- [x] Add `rcParams` pointer to `param_block_manager.h/.cpp`
- [x] Add settings to `settings_manager.cpp`
- [x] Sync RC settings to `rcParams` in `renderer.cpp` `beginFrame()`
- [x] Add ImGui controls in `renderer.cpp`

## Step 2: Create GPU Buffers

- [x] Add static `ComPtr<ID3D12Resource>` declarations for `dev_rcHashEntries`, `dev_rcAccumulation`, `dev_rcResolved`
- [x] Create buffers in `init()` using `createBasicBuffer` with UAV flag (after `initPipeline()`)
- [x] Add `.Reset()` calls in `destroy()`

## Step 3: Eviction/Clear Compute Shader + Pipeline

- [x] Add `RC_REGISTER_SPACE`, `RC_REGISTER_HASH_ENTRIES`, `RC_REGISTER_ACCUMULATION`, `RC_REGISTER_RESOLVED` to `common_registers.h` (space 4, u0/u1/u2)
- [x] Create `RcEvictParam` enum and `RC_EVICT_PARAM_IDX` macro in `renderer.cpp`
- [x] Build `rcEvictRootSig` in `initRootSignature()` (CBV + 3 UAVs)
- [x] Create `rc_evict.cs.hlsl` shader (evicts stale entries, zeros accumulation)
- [x] Create `rcEvictPso` in `initPipeline()`
- [x] Add eviction dispatch in render loop (between GBuffer barrier and path tracing)
- [x] Add UAV barrier after dispatch
- [x] Add `.Reset()` calls in `destroy()` for `rcEvictPso` and `rcEvictRootSig`

## Step 4: Resolve Compute Shader + Pipeline

- [x] Add `RC_RESOLVE_PARAM_IDX` macro (reuses `RcEvictParam` enum since layout is identical)
- [x] Create `rc_resolve.cs.hlsl` shader (blends accumulation into resolved buffer with EMA)
- [x] Create `rcResolvePso` in `initPipeline()` (reuses `rcEvictRootSig`)
- [x] Add resolve dispatch in render loop (after evict barrier, before path tracing)
- [x] Add UAV barrier after resolve dispatch
- [x] Add `.Reset()` call in `destroy()` for `rcResolvePso`
- [x] Add `#include "rc_resolve.cs.fxh"` for shader bytecode

## Step 5: Radiance Cache Utility Header (HLSL)

- [x] Add `RC_JITTER_SCALE` and `RC_EMPTY_SENTINEL` constants to `common_settings.h`
- [x] Create `radiance_cache.hlsli` with utility functions (`rcWorldToGrid`, `rcSpatialHash`, `rcPackKey`, `rcInsertOrFind`, `rcLookup`, `rcJitterPos`, `rcWriteRadiance`)
- [x] Switch `rcHashEntries` from `RWStructuredBuffer<uint2>` to `RWByteAddressBuffer` in `rc_evict.cs.hlsl` and `rc_resolve.cs.hlsl`
- [x] Switch `rcAccumulation` from `RWStructuredBuffer<uint4>` to `RWByteAddressBuffer` in `rc_evict.cs.hlsl` and `rc_resolve.cs.hlsl`
- [x] Update all reads/writes to use `.Load2`/`.Store2`/`.Load4`/`.Store4`
- [x] Change empty sentinel from `uint2(0, 0)` to `uint2(RC_EMPTY_SENTINEL, RC_EMPTY_SENTINEL)`

## Step 6: RC Update Pass (Cache Training)

- [x] Create `rc_update.rgs.hlsl` (defines `RC_UPDATE 1`, includes `path_tracing.rgs.hlsl`)
- [x] Add `#include "rc_update.rgs.fxh"` in `renderer.cpp`
- [x] Guard PT output UAVs with `#ifndef RC_UPDATE`, add RC UAVs with `#ifdef RC_UPDATE` in `path_tracing.rgs.hlsl`
- [x] Add `firstDiffusePos_WS` and `hasFirstDiffusePos` out params to `pathTraceRay` under `#ifdef RC_UPDATE`
- [x] Reset `pathColor` and `pathWeight` at first diffuse bounce under `#ifdef RC_UPDATE`
- [x] Guard path splitting block with `#ifndef RC_UPDATE`
- [x] Override `getPathSplitIdx()` to return 0 under `#ifdef RC_UPDATE` in `path_tracing_common.hlsli`
- [x] Add RC_UPDATE `RayGeneration()` branch: tile-based dispatch, random pixel selection, cache write via `rcInsertOrFind`/`rcWriteRadiance`
- [x] Add `RcUpdateParam` enum and `RC_UPDATE_PARAM_IDX` macro
- [x] Add `rcUpdateRootSig` (same as PT but with RC UAVs instead of PT output UAVs)
- [x] Add `rcUpdatePso`, `dev_rcUpdateShaderIds`, `rcUpdateDispatchDesc` static declarations
- [x] Create RC update PSO in `initPipeline()` with 3 hit groups
- [x] Restructure render loop: evict → update (raytracing) → resolve (moved after update)
- [x] Rebind rcCompute root sig and resources before resolve (since rcUpdate changed the root sig)
- [x] Add `.Reset()` calls in `destroy()` for `rcUpdatePso`, `rcUpdateRootSig`, `dev_rcUpdateShaderIds`

## Step 7: Debug Visualization

- [x] Add `rcDebugView` + 3 padding uints to `RadianceCacheParams` in `common_params.h` (keeps struct 32 bytes)
- [x] Add `rcDebugView` setting to `settings_manager.cpp`
- [x] Extend `PostprocessParam` enum with `RC_HASH_ENTRIES` and `RC_RESOLVED`
- [x] Add two SRV root params to `postprocessRootSig` in `initRootSignature()` (reuses `RC_REGISTER_SPACE`, `t0`/`t2`)
- [x] Sync `rcDebugView` from settings to `rcParams` in `render()`
- [x] Add pre-draw state transitions (UAV→SRV) + `SetGraphicsRootShaderResourceView` bindings, guarded by `rcParams->rcEnabled`
- [x] Add post-draw state transitions (SRV→UAV), guarded by `rcParams->rcEnabled`
- [x] Add `rcDebugViewComboOptions` and `ComboUint("RC debug view", ...)` to ImGui RC section
- [x] Include `radiance_cache.hlsli` in `postprocess.ps.hlsl`, declare `rcHashEntries` and `rcResolved` SRVs
- [x] Add `reconstructWorldPos` helper (linear depth + `getPrimaryRayDirection`)
- [x] Add `getRcDebugColor` helper (mode 1 = grid cell hash color, mode 2 = cached radiance lookup)
- [x] Add early-out in `psMain` for `rcParams.rcDebugView != 0` with TODO comment for future debug pass extraction

## Step 9: Cascading Voxel Sizes

- [x] Add `RC_TARGET_PIXEL_WIDTH`, `RC_MIN_LEVEL`, `RC_MAX_LEVEL`, `RC_LEVEL_OFFSET` to `common_settings.h`
- [x] Replace `float rcVoxelSize` with `float rcCascadeScale` in `RadianceCacheParams` (`common_params.h`)
- [x] Add `rcGetLevel(pos_WS)` helper — `clamp(floor(log2(dist * rcCascadeScale)), RC_MIN_LEVEL, RC_MAX_LEVEL)`
- [x] Add `rcGetVoxelSize(level)` helper — `exp2(float(level))`
- [x] Update `rcWorldToGrid` to take `int level`; use `floor(pos / voxelSize + 0.5)` to center cells at multiples of `voxelSize`
- [x] Update `rcSpatialHash` to take `int level` and XOR it into the hash
- [x] Update `rcPackKey` to encode level in 4 bits (new layout: 20+12 / 4+20+4+4 bits across `key.x`/`key.y`)
- [x] Update `rcInsertOrFind`, `rcLookup`, `rcJitterPos` signatures to take `int level`
- [x] Update RC_UPDATE branch in `path_tracing.rgs.hlsl` to call `rcGetLevel` and pass level
- [x] Update both debug views in `postprocess.ps.hlsl` to call `rcGetLevel` and pass level
- [x] Remove `rcVoxelSize` setting from `settings_manager.cpp`
- [x] Remove "RC voxel size" ImGui slider from `renderer.cpp`
- [x] Compute `rcCascadeScale = RC_TARGET_PIXEL_WIDTH * 2 * atan(tanHalfFovY) / renderHeight` each frame in `renderer.cpp`

## Step 10: Offset-Aware Grid Invalidation Fix

- [x] Modify `rcWorldToGrid` in `radiance_cache.hlsli` to produce absolute grid positions using `globalInstanceOffset`
- [x] For level > 0: decompose offset into integer grid offset (`offset >> level`) and small fractional correction (`offset & mask / voxelSize`)
- [x] For level <= 0: use exact integer shift (`offset << (-level)`) with no fractional part needed
- [x] All callers (`path_tracing.rgs.hlsl`, `postprocess.ps.hlsl`) automatically get the fix via `rcWorldToGrid`

## Step 12: Dedicated Debug View Pass + Unified Debug View Dropdown

- [x] Create `debug_view.ps.hlsl` with `getRcDebugColor`, `getDebugOutputColor`, and `psMain` dispatching between them; references `debugParams.rcDebugView` instead of `rcParams.rcDebugView`
- [x] Simplify `postprocess.ps.hlsl` to only `getPathTracingFinalColor` + NaN-guard `psMain`; remove RC includes, SRV declarations, and debug view logic
- [x] Move `rcDebugView` from `RadianceCacheParams` to `DebugParams` in `common_params.h`
- [x] Remove `rcDebugView` setting from `settings_manager.cpp`
- [x] Simplify `PostprocessParam` enum to `{ GLOBAL_PARAMS, COUNT }`; add `DebugViewParam` enum with RC SRV params and `DEBUG_VIEW_PARAM_IDX` macro
- [x] Add `debugViewRootSig` (CBV + RC SRVs + static sampler) and simplify `postprocessRootSig` (CBV + static sampler only)
- [x] Add `debugViewPso` (same VS as postprocess, new `debug_view.ps` PS)
- [x] Remove RC debug view ImGui combo from Radiance Cache section; extend `debugViewComboOptions` with `"rcGridCells"` and `"rcCachedRadiance"`; add both to `debugViewComboMap` mapping to `nullptr`
- [x] Derive `debugParams->rcDebugView` from the unified `debugView` setting string in `render()`
- [x] Replace single unconditional postprocess draw with conditional: debug view PSO when `isAnyDebugViewActive`, postprocess PSO otherwise
- [x] RC buffer transitions (UAV↔SRV) now guarded by `rcDebugActive` only (not `rcEnabled`), using `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` only
- [x] Add `debugViewPso.Reset()` and `debugViewRootSig.Reset()` in `destroy()`

## Findings

- **Do not build or test.** The user will build and test themselves. Future agents must not attempt to run cmake or build commands.
- RC buffers are resolution-independent (fixed at `RC_TABLE_SIZE`), so they are created once in `init()` rather than in `resize()`.
- Removed `rcDecay` setting — decay is controlled by the compile-time `RC_DECAY` constant in `common_settings.h`. No need for a runtime setting since the shader uses the constant directly.
- Evict and resolve dispatches are guarded by `if (rcParams->rcEnabled)` to skip GPU work when the radiance cache is disabled.
