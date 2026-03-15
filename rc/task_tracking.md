# Radiance Cache Implementation - Task Tracking

## Status Overview

| Step | Description | Status |
|------|-------------|--------|
| 1 | Add RC Constants, Params, and Settings | Done |
| 2 | Create GPU Buffers | Done |
| 3 | Eviction/Clear Compute Shader + Pipeline | Done |
| 4 | Resolve Compute Shader + Pipeline | Done |
| 5 | Radiance Cache Utility Header (HLSL) | Not Started |
| 6 | RC Update Pass (Cache Training) | Not Started |
| 7 | Debug Visualization | Not Started |
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

## Findings

- **Do not build or test.** The user will build and test themselves. Future agents must not attempt to run cmake or build commands.
- RC buffers are resolution-independent (fixed at `RC_TABLE_SIZE`), so they are created once in `init()` rather than in `resize()`.
- Removed `rcDecay` setting — decay is controlled by the compile-time `RC_DECAY` constant in `common_settings.h`. No need for a runtime setting since the shader uses the constant directly.
- Evict and resolve dispatches are guarded by `if (rcParams->rcEnabled)` to skip GPU work when the radiance cache is disabled.
