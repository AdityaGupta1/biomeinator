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
| 9 | Tuning and Polish | Not Started |

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
- [x] Add `reconstructWorldPos` helper (linear depth + camera basis vectors)
- [x] Add `getRcDebugColor` helper (mode 1 = grid cell hash color, mode 2 = cached radiance lookup)
- [x] Add early-out in `psMain` for `rcParams.rcDebugView != 0` with TODO comment for future debug pass extraction

## Findings

- **Do not build or test.** The user will build and test themselves. Future agents must not attempt to run cmake or build commands.
- RC buffers are resolution-independent (fixed at `RC_TABLE_SIZE`), so they are created once in `init()` rather than in `resize()`.
- Removed `rcDecay` setting — decay is controlled by the compile-time `RC_DECAY` constant in `common_settings.h`. No need for a runtime setting since the shader uses the constant directly.
- Evict and resolve dispatches are guarded by `if (rcParams->rcEnabled)` to skip GPU work when the radiance cache is disabled.
