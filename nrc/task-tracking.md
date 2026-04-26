# NRC Integration Task Tracking

See [integration-plan.md](integration-plan.md) for full details on each step.

## Status legend

- **pending** -- not yet started
- **in progress** -- actively being worked on
- **blocked** -- waiting on something
- **deferred** -- intentionally moved to a later step
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
| A6 | Scene bounds are available from `voxelBoundsMin/Max_WS` in voxel mode. glTF mode still uses broad placeholder bounds until `Scene` exposes a loaded-scene AABB. | revised |
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
| 2.4 Wire `initNrc()` / `destroyNrc()` to the cache toggle | done | Uses `nrcEnabled`; old `rcEnabled` was removed with the legacy RC path. |
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
| 4.3 Replace old RC update blocks with NRC API calls in `path_tracing.rgs.hlsl` | done | NRC variants use SDK shader API; legacy `RC_UPDATE` code was removed during cleanup. |
| 4.4 Replace RC lookup termination with `NrcProgressState` handling | done | `TerminateImmediately` and `TerminateAfterDirectLighting` handled in the bounce loop |
| 4.5 Add `NrcSurfaceAttributes` population from decoded material | done | Position, normal, roughness, F0, diffuse reflectance, view vector, and delta-lobe state populated. Query pass applies first-bounce path splitting before `NrcUpdateOnHit()` so NRC sees the actual split lobe and adjusted throughput. |
| 4.6 Add `NrcSetBrdfPdf` call after BSDF sampling | done | Also updates NRC's previous-hit delta flag from the sampled BSDF lobe so mixed diffuse+glossy materials do not let specular reflection paths trigger premature NRC queries. |
| 4.7 Gate Russian roulette with `NrcCanUseRussianRoulette` | done | |
| 4.8 Add `NrcWriteFinalPathInfo` after bounce loop | done | Early NRC exits were tightened so created path states reach final path info writes |
| 4.9 Defer old RC shader deletion | done | Historical Step 4 staging choice; old RC shaders were deleted in Step 7. |
| 4.10 Update `shaders.cpp` for NRC shader variants | done | NRC shaders added; old RC shader registrations removed in cleanup. |
| 4.11 Build new PSOs for NRC update and query variants | done | Query dispatch now uses `nrcQueryDispatchDesc` instead of the plain PT shader table |
| 4.12 Bind NRC buffers from C++ before DispatchRays | done | Uses `nrcContext->GetBuffers()` for update and query dispatches |

### Step 5: QueryAndTrain and Resolve -- done

| Task | Status | Notes |
|------|--------|-------|
| 5.1 Call `QueryAndTrain` after query dispatch | done | App descriptor heap is restored immediately after the SDK call because NRC binds its own heap internally. |
| 5.2 Create custom resolve compute shader (`nrc_resolve.cs.hlsl`) | done | Reads QueryPathInfo + QueryRadiance and writes to `dev_pathTracingRawBuffer`. NRC SDK buffers stay bound as UAV root descriptors. |
| 5.3 Create root signature and PSO for custom resolve | done | Uses NrcConstants (CBV), QueryPathInfo (UAV), QueryRadiance (UAV), raw buffer (UAV). |
| 5.4 Dispatch custom resolve after QueryAndTrain, before Collect | done | Dispatches at `frameDimensions` (doubled width when path splitting is on). |
| 5.5 Create debug resolve texture + use built-in `Resolve()` for debug modes | done | Completed in Step 6 debug/UI work; normal NRC rendering uses the custom resolve. |
| 5.6 Call `EndFrame` after command list submission | done | Called after `submitCmd()` while the frame's NRC context is still active. |
| 5.7 Remove old RC sub-passes (evict, update, resolve dispatches) | done | Old RC runtime passes and transitions were removed from the render loop. |
| 5.8 Remove old RC resources and root signatures | done | Removed old RC GPU resources, root signatures, PSOs, shader table, and param enums from `renderer.cpp`. |
| 5.9 Remove old RC params from param block and settings | done | Removed `rcEnabled`, `rcMinSamplesForQuery`, and RC debug settings; only padding remains in the shared struct layout. |
| 5.10 Update `configureNrc()` to set `frameDimensions` based on `doPathSplitting` | done | `frameDimensions = { renderWidth * (doPathSplitting ? 2 : 1), renderHeight }` |
| 5.11 Wire up the full NRC pass sequence in the render loop | done | BeginFrame -> G-Buffer -> NRC Update -> NRC Query -> QueryAndTrain -> restore app descriptor heap -> Custom Resolve -> Collect -> EndFrame. |

### Step 6: Debug views and UI polish — done

| Task | Status | Notes |
|------|--------|-------|
| 6.1 Add NRC resolve mode combo box to ImGui | done | Uses raw `ImGui::Combo` with `nrc::GetImGuiResolveModeComboString()`. Stored as uint setting `nrcResolveMode`. Only shown when NRC is enabled. |
| 6.2 Switch between custom resolve (normal mode) and built-in resolve + debug texture (debug modes) | done | `nrcDebugTarget` RtTarget created at `frameDimensions` (path-splitting-aware). Built-in `Resolve()` used for debug modes; custom resolve for normal mode. Debug texture overrides the debug view target when active. Descriptor heap restored after SDK `Resolve()` call. |
| 6.3 Remove old RC debug view from `debug_view.ps.hlsl` | done | No-op — no old RC debug view references found in `debug_view.ps.hlsl` or the debug view pass. Already cleaned up during Step 5. |
| 6.4 Add NRC tuning sliders (threshold, radiance scale, etc.) | done | Added 7 settings: `nrcResolveMode`, `nrcMaxRadiance`, `nrcTerminationThreshold`, `nrcTrainingTerminationThreshold`, `nrcSkipDeltaVertices`, `nrcTrainTheCache`, `nrcLearningRate`. All exposed through ImGui when NRC is enabled. |
| 6.5 Ensure accumulation resets on NRC toggle / setting change | done | All NRC setting changes OR into `didPathTracingSettingsChange`, which drives `resetAccumulation`. Covered by construction from 6.1 and 6.4. |

### Step 6 deviations from plan

- **NRC context destroyed and recreated on resize instead of just reconfigured.** Calling
  `configureNrc()` alone after a resize (e.g. path splitting toggle) caused crashes when a
  debug resolve mode was active. The SDK's built-in `Resolve()` for debug modes uses
  internal debug buffers that are not properly reallocated by `Configure()` alone. The fix
  is to call `destroyNrc()` + `initNrc()` in `resize()`, which fully recreates the context
  and its debug buffers at the correct `frameDimensions`.

- **Task 6.3 was a no-op.** No old RC debug view references existed in `debug_view.ps.hlsl`
  or the debug view pass — already cleaned up during Step 5.

### Step 7: Cleanup and knowledgebase — done

| Task | Status | Notes |
|------|--------|-------|
| 7.1 Delete `radiance_cache.hlsli`, `rc_evict.cs.hlsl`, `rc_resolve.cs.hlsl` | done | Also deleted `rc_update.rgs.hlsl` |
| 7.2 Remove RC defines from `common_settings.h` | done | `RC_TABLE_SIZE`, `RC_WORKGROUP_SIZE`, `RC_UPDATE_SCALE`, `RC_TARGET_PIXEL_WIDTH` |
| 7.3 Grep for leftover RC references in `src/` | done | Removed legacy RC shader code, params, settings, resources, and register definitions. Remaining `NRC_*` symbols are the active NRC implementation. |
| 7.4 Update knowledgebase entries | done | Rewrote `radiance_cache.md` for NRC, updated `render_passes.md`, `path_tracing.md`, shader and rendering `index.md` files |
