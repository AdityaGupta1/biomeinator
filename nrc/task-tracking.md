# NRC Integration Task Tracking

See [integration-plan.md](integration-plan.md) for full details on each step.

## Status legend

- **pending** -- not yet started
- **in progress** -- actively being worked on
- **blocked** -- waiting on something
- **done** -- complete and verified

## Assumptions

Assumptions that the plan is built on. Update this section if any change during
implementation.

| # | Assumption | Status |
|---|-----------|--------|
| A1 | NRC SDK manages its own GPU buffers (`enableGPUMemoryAllocation = true`) | active |
| A2 | Path splitting is disabled when NRC is active (mutually exclusive for now) | active |
| A3 | Built-in `Resolve()` is used (no custom resolve pass needed initially) | active |
| A4 | `dev_pathTracingRawBuffer` is format-compatible with NRC's `Resolve` output | unverified |
| A5 | `flush()` is acceptable when toggling NRC off at runtime | active |
| A6 | Scene bounds are available from the scene AABB (glTF) or `voxelBoundsMin/Max_WS` (voxel mode) | active |
| A7 | Three shader variants (no-NRC, NRC update, NRC query) compiled at build time; runtime toggle selects which PSO to dispatch | active |

## Tasks

### Step 1: Build system and linking

| Task | Status | Notes |
|------|--------|-------|
| 1.1 Add `external/NRC/Include` to C++ include paths | done | |
| 1.2 Link against `external/NRC/Lib/NRC_D3D12.lib` | done | |
| 1.3 Copy NRC DLLs to output directory (post-build) | done | cudart64_12.dll, nvrtc64_120_0.dll, nvrtc-builtins64_128.dll also copied |
| 1.4 Add `-I external/NRC/Include` to DXC shader compilation | done | |
| 1.5 Add `#include "NrcD3d12.h"` to `renderer.cpp` and verify it compiles | done | |

### Step 2: NRC context lifecycle

| Task | Status | Notes |
|------|--------|-------|
| 2.1 Add NRC state variables (`nrcContext`, `nrcInitialized`) | pending | |
| 2.2 Implement `initNrc()` (Initialize + Create + Configure) | pending | |
| 2.3 Implement `destroyNrc()` (flush + Destroy + Shutdown) | pending | |
| 2.4 Wire `initNrc()` / `destroyNrc()` to the `rcEnabled` toggle | pending | Replaces `initRadianceCache()` |
| 2.5 Handle reconfiguration on resolution change | pending | |
| 2.6 Wire `initNrc()` into startup, `destroyNrc()` into `destroy()` | pending | |

### Step 3: NrcConstants in constant buffer

| Task | Status | Notes |
|------|--------|-------|
| 3.1 Add `NrcConstants` to param block / constant buffer layout | pending | Must be 16-byte aligned |
| 3.2 Call `BeginFrame` + `PopulateShaderConstants` each frame | pending | |
| 3.3 Declare `NrcConstants` on the shader side | pending | |

### Step 4: Shader-side NRC integration

| Task | Status | Notes |
|------|--------|-------|
| 4.1 Create `nrc_update.rgs.hlsl` and `nrc_query.rgs.hlsl` | pending | |
| 4.2 Add NRC buffer bindings (root params / UAVs) to PT root sig | pending | 5 buffers: QueryPathInfo, TrainingPathInfo, TrainingPathVertices, QueryRadianceParams, Counter |
| 4.3 Replace `#ifdef RC_UPDATE` blocks with NRC API calls in `path_tracing.rgs.hlsl` | pending | Largest single task |
| 4.4 Replace RC lookup termination with `NrcProgressState` handling | pending | |
| 4.5 Add `NrcSurfaceAttributes` population from decoded material | pending | Map existing material fields to NRC's expected inputs |
| 4.6 Add `NrcSetBrdfPdf` call after BSDF sampling | pending | |
| 4.7 Gate Russian roulette with `NrcCanUseRussianRoulette` | pending | |
| 4.8 Add `NrcWriteFinalPathInfo` after bounce loop | pending | |
| 4.9 Delete `rc_update.rgs.hlsl` | pending | |
| 4.10 Update `shaders.cpp` (remove old RC shaders, add NRC shaders) | pending | |
| 4.11 Build new PSOs for NRC update and query variants | pending | |
| 4.12 Bind NRC buffers from C++ before DispatchRays | pending | Use `nrcContext->GetBuffers()` |

### Step 5: QueryAndTrain and Resolve

| Task | Status | Notes |
|------|--------|-------|
| 5.1 Call `QueryAndTrain` after query dispatch | pending | |
| 5.2 Call `Resolve` with output buffer | pending | Verify A4 (buffer format compatibility) |
| 5.3 Call `EndFrame` after command list submission | pending | |
| 5.4 Remove old RC sub-passes (evict, update, resolve dispatches) | pending | |
| 5.5 Remove old RC resources and root signatures | pending | |
| 5.6 Remove old RC params from param block and settings | pending | |
| 5.7 Force `doPathSplitting = false` when NRC is active | pending | |
| 5.8 Wire up the full NRC pass sequence in the render loop | pending | BeginFrame -> G-Buffer -> NRC Update -> NRC Query -> QueryAndTrain -> Resolve -> Collect -> ... -> EndFrame |

### Step 6: Debug views and UI polish

| Task | Status | Notes |
|------|--------|-------|
| 6.1 Add NRC resolve mode combo box to ImGui | pending | Use `GetImGuiResolveModeComboString()` |
| 6.2 Remove old RC debug view from `debug_view.ps.hlsl` | pending | |
| 6.3 Add NRC tuning sliders (threshold, radiance scale, etc.) | pending | |
| 6.4 Ensure accumulation resets on NRC toggle / setting change | pending | |

### Step 7: Cleanup and knowledgebase

| Task | Status | Notes |
|------|--------|-------|
| 7.1 Delete `radiance_cache.hlsli`, `rc_evict.cs.hlsl`, `rc_resolve.cs.hlsl` | pending | |
| 7.2 Remove RC defines from `common_settings.h` | pending | `RC_TABLE_SIZE`, `RC_WORKGROUP_SIZE`, `RC_UPDATE_SCALE` |
| 7.3 Grep for leftover RC references in `src/` | pending | |
| 7.4 Update knowledgebase entries | pending | `radiance_cache.md`, `render_passes.md`, `path_tracing.md`, `index.md` |
