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
| A2 | Path splitting works with NRC. `frameDimensions` is set to `(renderWidth * (doPathSplitting ? 2 : 1), renderHeight)` to match the doubled query dispatch. NRC context creation uses raw `DispatchRaysIndex().xy`, while normal shading still uses `getPixelIdx()` for path-split pixel semantics. The custom resolve indexes linearly at the doubled width, which matches the interleaved buffer layout. | active |
| A3 | ~~Built-in `Resolve()` is used~~ — **revised**: custom resolve compute shader needed because built-in resolve expects a texture output (Vulkan API requires `VkImageView`; D3D12 likely same internally), but our output is a structured buffer | revised |
| A4 | `dev_pathTracingRawBuffer` is format-compatible with NRC's `Resolve` output | N/A — bypassed by custom resolve |
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
| 2.1 Add NRC state variables (`nrcContext`, `nrcInitialized`) | done | `nrcInitialized` skipped — `nrcContext != nullptr` serves the same purpose |
| 2.2 Implement `initNrc()` (Initialize + Create + Configure) | done | Configure call consolidated into `configureNrc()` helper. Guarded against duplicate init if NRC starts enabled. |
| 2.3 Implement `destroyNrc()` (flush + Destroy + Shutdown) | done | |
| 2.4 Wire `initNrc()` / `destroyNrc()` to the `rcEnabled` toggle | done | Uses separate `nrcEnabled` setting so old RC can coexist during migration |
| 2.5 Handle reconfiguration on resolution change | done | Also reconfigures on `maxPathDepth` and `doPathSplitting` changes. `doPathSplitting` triggers resize → `configureNrc()`, which now reads `doPathSplitting` to set `frameDimensions`. |
| 2.6 Wire `initNrc()` into startup, `destroyNrc()` into `destroy()` | done | |

### Step 3: NrcConstants in constant buffer

| Task | Status | Notes |
|------|--------|-------|
| 3.1 Add `NrcConstants` to param block / constant buffer layout | done | Placed after `debugParams` in the upload buffer (outside `GlobalParams` cbuffer). `getNrcConstantsGpuAddress()` returns its GPU VA. |
| 3.2 Call `BeginFrame` + `PopulateShaderConstants` each frame | done | Called after NRC toggle/reconfigure logic. Zero-initialized when NRC is disabled. |
| 3.3 Bind as separate root CBV in path tracing root signature | done | Added `NRC_CONSTANTS` to `PtParam` enum, root sig, and bind call. Register space 5, b0. Shader-side cbuffer declaration deferred to step 4. |

### Step 4: Shader-side NRC integration — done

| Task | Status | Notes |
|------|--------|-------|
| 4.1 Create `nrc_update.rgs.hlsl` and `nrc_query.rgs.hlsl` | done | Thin wrapper variants around `path_tracing.rgs.hlsl` |
| 4.2 Add NRC buffer bindings (root params / UAVs) to PT root sig | done | 5 buffers: QueryPathInfo, TrainingPathInfo, TrainingPathVertices, QueryRadianceParams, Counter |
| 4.3 Replace `#ifdef RC_UPDATE` blocks with NRC API calls in `path_tracing.rgs.hlsl` | done | NRC variants use SDK shader API; old RC code remains for `RC_UPDATE` during coexistence |
| 4.4 Replace RC lookup termination with `NrcProgressState` handling | done | `TerminateImmediately` and `TerminateAfterDirectLighting` handled in the bounce loop |
| 4.5 Add `NrcSurfaceAttributes` population from decoded material | done | Position, normal, roughness, F0, diffuse reflectance, view vector, and delta-lobe state populated |
| 4.6 Add `NrcSetBrdfPdf` call after BSDF sampling | done | |
| 4.7 Gate Russian roulette with `NrcCanUseRussianRoulette` | done | |
| 4.8 Add `NrcWriteFinalPathInfo` after bounce loop | done | Early NRC exits were tightened so created path states reach final path info writes |
| 4.9 Defer old RC shader deletion | done | `rc_update.rgs.hlsl` and old RC shaders intentionally remain while `rcEnabled` and `nrcEnabled` coexist |
| 4.10 Update `shaders.cpp` for NRC shader variants | done | NRC shaders added; old RC shader registrations retained for coexistence |
| 4.11 Build new PSOs for NRC update and query variants | done | Query dispatch now uses `nrcQueryDispatchDesc` instead of the plain PT shader table |
| 4.12 Bind NRC buffers from C++ before DispatchRays | done | Uses `nrcContext->GetBuffers()` for update and query dispatches |

### Step 5: QueryAndTrain and Resolve

| Task | Status | Notes |
|------|--------|-------|
| 5.1 Call `QueryAndTrain` after query dispatch | pending | |
| 5.2 Create custom resolve compute shader (`nrc_resolve.cs.hlsl`) | pending | ~15 lines HLSL; reads QueryPathInfo + QueryRadiance, writes to `dev_pathTracingRawBuffer` |
| 5.3 Create root signature and PSO for custom resolve | pending | Needs: NrcConstants (CBV), QueryPathInfo (SRV), QueryRadiance (SRV), raw buffer (UAV) |
| 5.4 Dispatch custom resolve after QueryAndTrain, before Collect | pending | Dispatch at `frameDimensions` (doubled width when path splitting is on) |
| 5.5 Create debug resolve texture + use built-in `Resolve()` for debug modes | pending | Temporary `RWTexture2D<float4>` for heatmaps, cache view, etc. |
| 5.6 Call `EndFrame` after command list submission | pending | |
| 5.7 Remove old RC sub-passes (evict, update, resolve dispatches) | pending | |
| 5.8 Remove old RC resources and root signatures | pending | |
| 5.9 Remove old RC params from param block and settings | pending | |
| 5.10 Update `configureNrc()` to set `frameDimensions` based on `doPathSplitting` | done | `frameDimensions = { renderWidth * (doPathSplitting ? 2 : 1), renderHeight }` |
| 5.11 Wire up the full NRC pass sequence in the render loop | pending | BeginFrame -> G-Buffer -> NRC Update -> NRC Query -> QueryAndTrain -> Custom Resolve -> Collect -> ... -> EndFrame |

### Step 6: Debug views and UI polish

| Task | Status | Notes |
|------|--------|-------|
| 6.1 Add NRC resolve mode combo box to ImGui | pending | Use `GetImGuiResolveModeComboString()` |
| 6.2 Switch between custom resolve (normal mode) and built-in resolve + debug texture (debug modes) | pending | Built-in `Resolve()` targets a temp `RWTexture2D<float4>` for debug visualization |
| 6.3 Remove old RC debug view from `debug_view.ps.hlsl` | pending | |
| 6.4 Add NRC tuning sliders (threshold, radiance scale, etc.) | pending | |
| 6.5 Ensure accumulation resets on NRC toggle / setting change | pending | |

### Step 7: Cleanup and knowledgebase

| Task | Status | Notes |
|------|--------|-------|
| 7.1 Delete `radiance_cache.hlsli`, `rc_evict.cs.hlsl`, `rc_resolve.cs.hlsl` | pending | |
| 7.2 Remove RC defines from `common_settings.h` | pending | `RC_TABLE_SIZE`, `RC_WORKGROUP_SIZE`, `RC_UPDATE_SCALE` |
| 7.3 Grep for leftover RC references in `src/` | pending | |
| 7.4 Update knowledgebase entries | pending | `radiance_cache.md`, `render_passes.md`, `path_tracing.md`, `index.md` |
