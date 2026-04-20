# NRC Integration Plan

Replacing the custom hash-grid radiance cache with NVIDIA's Neural Radiance Cache (NRC).

## Background

**Current system**: Three sub-passes (evict, update, resolve) operating on a world-space
hash table (`dev_rcHashEntries`, `dev_rcAccumulation`, `dev_rcResolved`). The path tracing
shader is compiled twice: normally for the query pass and with `RC_UPDATE` for the update
pass. The RC is position-only (no view dependence) and is toggled at runtime via
`rcEnabled`.

**NRC**: A binary library (DLL + headers) that trains a neural network each frame to
predict radiance given position + direction. Provides both a C++ API (`NrcD3d12.h`) and a
shader API (`Nrc.hlsli`). Buffers are managed internally by the library.

## Design decisions

- **NRC memory management**: Let the SDK manage its own GPU buffers
  (`enableGPUMemoryAllocation = true`). This avoids having to replicate the allocation
  logic from `NrcCommon.h` / `BuffersAllocationInfo`.

- **Runtime toggle**: The UI checkbox "Enable radiance cache" (`rcEnabled`) will be
  repurposed to toggle NRC. When disabled, the path tracer runs a single full-res dispatch
  with no NRC calls. When enabled, the full NRC pipeline runs (update dispatch, query
  dispatch, QueryAndTrain, Resolve). Toggling on requires creating the NRC context +
  calling `Configure`. Toggling off requires `flush()` + `nrc::d3d12::Context::Destroy`.
  This matches the current pattern where toggling off already calls `flush()`.

- **Shader compilation**: Three shader variants of `path_tracing.rgs.hlsl`:
  1. `path_tracing.rgs.hlsl` (no NRC defines) -- used when NRC is disabled.
  2. `nrc_update.rgs.hlsl` (`#define NRC_UPDATE 1`, then `#include "path_tracing.rgs.hlsl"`)
     -- replaces `rc_update.rgs.hlsl`.
  3. `nrc_query.rgs.hlsl` (`#define NRC_QUERY 1`, then `#include "path_tracing.rgs.hlsl"`)
     -- used for the full-res query pass when NRC is on.

- **Resolve approach**: Use NRC's built-in `Resolve()` initially. The built-in resolve
  adds query radiance to a combined output buffer. Path splitting (`doPathSplitting`)
  should be disabled when NRC is active (the two features are mutually exclusive for now),
  since NRC's resolve writes to a single buffer. A custom resolve can be added later if
  path splitting + NRC is needed.

- **NRC include path**: The shader compiler needs `-I external/NRC/Include` added to its
  DXC invocation so that `#include "Nrc.hlsli"` resolves correctly. The NRC shader headers
  also need the include path for `common_settings.h` etc. to work.

- **Scene bounds**: NRC requires `ContextSettings::sceneBoundsMin/Max`. For voxel mode,
  these come from `voxelBoundsMin/Max_WS`. For glTF mode, they should come from the
  loaded scene's AABB.

---

## Step 1: Build system and linking

**Goal**: The project compiles and links against the NRC library, and the NRC DLL is copied
to the output directory. No runtime behavior changes yet.

### Work

1. In `CMakeLists.txt`:
   - Add `external/NRC/Include` to the include paths.
   - Link against `external/NRC/Lib/NRC_D3D12.lib`.
   - Add a post-build step to copy `external/NRC/Bin/NRC_D3D12.dll` (and any CUDA DLLs in
     that directory) to the build output directory.
   - Add `-I ${CMAKE_SOURCE_DIR}/external/NRC/Include` to the DXC shader compile commands
     so `Nrc.hlsli` can be found.

2. In `renderer.cpp`, add `#include "NrcD3d12.h"` (guarded or unconditional) and verify it
   compiles.

### Verification

- Project builds without errors.
- `NRC_D3D12.dll` appears in the output directory.
- No runtime behavior changes -- the app launches and renders as before.

---

## Step 2: NRC context lifecycle (init / destroy / toggle) — DONE

**Goal**: The NRC context is created at startup (or when the toggle is flipped on) and
destroyed on shutdown (or when flipped off). No rendering changes yet -- NRC passes are
not dispatched.

### Work

1. Add NRC state variables to `renderer.cpp`:
   - `static nrc::d3d12::Context* nrcContext = nullptr;`
   - `static bool nrcInitialized = false;`

2. Write an `initNrc()` function:
   - Call `nrc::d3d12::Initialize(globalSettings)` (once, at app startup, before context
     creation). Set `enableGPUMemoryAllocation = true`, `enableDebugBuffers = true` (for
     debug resolve modes), and `maxNumFramesInFlight` to match your triple buffering count.
   - Call `nrc::d3d12::Context::Create(device5, nrcContext)`.
   - Call `nrcContext->Configure(contextSettings)` with:
     - `frameDimensions` = render resolution.
     - `trainingDimensions` = `nrc::ComputeIdealTrainingDimensions(frameDimensions, 0)`.
     - `maxPathVertices` = 8 (or `renderParams.maxPathDepth`).
     - `samplesPerPixel` = 1.
     - `sceneBoundsMin/Max` = scene AABB.
     - `includeDirectLighting` = false (we add direct lighting ourselves in the path
       tracer; NRC only provides indirect).
     - `learnIrradiance` = false.

3. Write a `destroyNrc()` function:
   - `flush()`, then `nrc::d3d12::Context::Destroy(*nrcContext)`, then
     `nrc::d3d12::Shutdown()`. Set `nrcContext = nullptr`.

4. Call `initNrc()` from the existing `init()` if `rcEnabled` is true at startup, mirroring
   the current `initRadianceCache()` call. Call `destroyNrc()` from `destroy()`.

5. In the per-frame section where `rcEnabled` is toggled:
   - On enable: call `initNrc()` (replaces `initRadianceCache()`).
   - On disable: call `destroyNrc()` (replaces `flush()` + buffer `.Reset()` calls).

6. Handle reconfiguration: if the render resolution changes while NRC is active, call
   `nrcContext->Configure(newContextSettings)` again. (this can likely be done in the resize() method)

### Verification

- Toggle the checkbox on/off in the UI -- no crash, no GPU hang.
- The app still renders the same image (NRC passes aren't dispatched yet, so the path
  tracer runs without a cache, which is the expected fallback).
- Startup and shutdown are clean (no leaked resources, no validation layer errors).

### Deviations from plan

- **`nrcInitialized` not added.** The plan called for a separate `nrcInitialized` flag,
  but `nrcContext != nullptr` already serves as the "is NRC active" check, so the extra
  bool was unnecessary.

- **Separate `nrcEnabled` setting instead of reusing `rcEnabled`.** NRC init/destroy is
  gated on `nrcEnabled` rather than `rcEnabled`, so both cache implementations can coexist
  during the migration. The old RC still uses `rcEnabled`. These will be consolidated once
  the old RC code is removed in step 5.

- **`configureNrc()` instead of raw `buildNrcContextSettings()` + `Configure()`.** The
  plan had `Configure(contextSettings)` called inline at each site. These were consolidated
  into a single `configureNrc()` function that builds the settings and calls `Configure()`
  internally, reducing duplication across the three call sites (init, resize, and
  per-frame `maxPathDepth` change).

- **Per-frame reconfiguration on `maxPathDepth` change.** The plan only mentioned
  reconfiguration on resolution change (item 6). The implementation also reconfigures when
  `maxPathDepth` changes at runtime, since that value feeds into
  `ContextSettings::maxPathVertices`.

---

## Step 3: Add NrcConstants to the constant buffer

**Goal**: `NrcConstants` is populated each frame and available to the path tracing shader
via the global constant buffer.

### Work

1. Add an `NrcConstants nrcConstants;` field to the param block structure in
   `common_params.h` (or alongside `rcParams`). It must be 16-byte aligned per HLSL
   constant buffer rules. Alternatively, replace the existing `RcParams` struct with
   `NrcConstants`.

2. Each frame, when NRC is enabled:
   - Call `nrcContext->BeginFrame(cmdList, frameSettings)` at the start of the frame,
     before any NRC-related GPU work. Populate `frameSettings` with:
     - `maxExpectedAverageRadianceValue` = a suitable value for your scene (start with 1.0).
     - `terminationHeuristicThreshold` = 0.1 (default).
     - `resolveMode` = `NrcResolveMode::AddQueryResultToOutput`.
   - Call `nrcContext->PopulateShaderConstants(nrcConstants)` and upload the result into
     the constant buffer.

3. On the shader side, declare `NrcConstants nrcConstants` accessible from the global
   constant buffer (similar to how `rcParams` is currently accessed).

### Verification

- Capture a frame in Nsight Graphics or PIX. Inspect the constant buffer and confirm
  `NrcConstants` fields are populated with sensible values (non-zero `frameDimensions`,
  `trainingDimensions`, etc.).
- No rendering changes yet.

---

## Step 4: Shader-side NRC integration (update and query variants)

**Goal**: The path tracing shader is modified to use NRC's shader API. Three shader
variants are compiled. NRC buffers are bound and written to.

### Work

1. **Create new shader entry files**:
   - `nrc_update.rgs.hlsl`: `#define NRC_UPDATE 1` then `#include "path_tracing.rgs.hlsl"`.
   - `nrc_query.rgs.hlsl`: `#define NRC_QUERY 1` then `#include "path_tracing.rgs.hlsl"`.
   - Delete `rc_update.rgs.hlsl`.

2. **Modify `path_tracing.rgs.hlsl`**:
   - Replace `#include "radiance_cache.hlsli"` with NRC includes:
     ```hlsl
     #define NRC_USE_CUSTOM_BUFFER_ACCESSORS 1
     #include "Nrc.hlsli"
     ```
   - Define the `NRC_BUFFER_*` macros to point at UAV bindings for the NRC buffers
     (queryPathInfo, trainingPathInfo, trainingPathVertices, queryRadianceParams,
     countersData). These are bound as root UAVs or via a descriptor table.
   - Replace the `#ifdef RC_UPDATE` blocks:
     - The `rcSlots[]` / `rcThroughputs[]` arrays and all `rcInsertOrFind` / `rcWriteRadiance`
       / `rcWriteSampleCount` calls are removed entirely.
     - At the top of `pathTraceRay()`, when NRC is enabled, create the NRC context and
       path state:
       ```hlsl
       NrcBuffers nrcBuffers = { ... };
       NrcContext nrcCtx = NrcCreateContext(nrcConstants, nrcBuffers, pixelIdx);
       NrcPathState nrcPathState = NrcCreatePathState(nrcConstants, rng.nextFloat());
       ```
     - At each hit (before BSDF sampling), populate `NrcSurfaceAttributes` from the
       decoded material and call `NrcUpdateOnHit(...)`. Check the returned
       `NrcProgressState`:
       - `TerminateImmediately` -> break out of the bounce loop.
       - `TerminateAfterDirectLighting` -> continue through direct lighting evaluation,
         then break.
       - `Continue` -> proceed normally.
     - On miss, call `NrcUpdateOnMiss(nrcPathState)`.
     - After BSDF sampling, call `NrcSetBrdfPdf(nrcPathState, pdf)`.
     - For Russian roulette, gate it with `NrcCanUseRussianRoulette(nrcPathState)`.
     - After the bounce loop, call `NrcWriteFinalPathInfo(nrcCtx, nrcPathState, throughput, radiance)`.
   - Remove the old RC lookup termination logic (`rcLookup` / `rcResolved` read).
   - When `NrcIsUpdateMode()`: cap samples per pixel to 1, dispatch at training resolution.
   - When `!NrcIsEnabled()`: the shader behaves as the plain path tracer (no cache).

3. **Bind NRC buffers from C++**:
   - Get the NRC buffers via `nrcContext->GetBuffers()`.
   - Add root parameters for the NRC UAV buffers to the path tracing root signature (for
     both update and query variants). The needed buffers are: `QueryPathInfo`,
     `TrainingPathInfo`, `TrainingPathVertices`, `QueryRadianceParams`, `Counter`.
   - Bind them before each DispatchRays call.

4. **Delete old RC shader files**: `rc_evict.cs.hlsl`, `rc_resolve.cs.hlsl`,
   `radiance_cache.hlsli`.

5. **Update `shaders.cpp`**: Remove the old RC shader registrations, add the new NRC
   update/query shader registrations.

6. **Update `CMakeLists.txt`**: The new `.rgs.hlsl` files will be auto-discovered by the
   existing glob. The deleted files will be auto-removed. No manual changes needed unless
   the glob doesn't cover the new filenames.

### Verification

- Project compiles with all three shader variants.
- With NRC disabled (checkbox off), the app renders identically to before (no cache, plain
  path tracer).
- With NRC enabled, the update and query dispatches run without GPU hangs. Use Nsight
  Graphics to capture a frame and verify:
  - The NRC Counter buffer has non-zero query and training record counts.
  - `TrainingPathVertices` contains plausible position/normal data.
  - `QueryRadianceParams` contains plausible position/direction data.
- The image will look wrong at this point because `QueryAndTrain` and `Resolve` haven't
  been called yet -- that's expected.

---

## Step 5: QueryAndTrain and Resolve passes

**Goal**: The NRC neural network is trained and queried each frame, and the predicted
radiance is resolved into the output buffer. The image should look correct with NRC
enabled.

### Work

1. In the render loop, after the query dispatch (which replaces the old path tracing
   dispatch), call:
   ```cpp
   nrcContext->QueryAndTrain(cmdList, nullptr); // nullptr = don't track training loss
   ```

2. Determine the output buffer for `Resolve`:
   - The current path tracer writes to `dev_pathTracingRawBuffer`. NRC's `Resolve` adds
     the predicted radiance to an output buffer.
   - `Resolve` should write to the same `dev_pathTracingRawBuffer` so that the existing
     Collect pass picks it up. Verify that the buffer format is compatible (NRC expects a
     UAV it can write `float4` to; the raw buffer is `float4` per pixel).
   - Call:
     ```cpp
     nrcContext->Resolve(cmdList, dev_pathTracingRawBuffer.Get());
     ```

3. After the command list is submitted, call:
   ```cpp
   nrcContext->EndFrame(graphicsCmdQueue.Get());
   ```

4. Remove the old RC sub-passes from the render loop:
   - Delete the RC Evict dispatch.
   - Delete the RC Update dispatch.
   - Delete the RC Resolve dispatch.
   - Delete the UAV/SRV transitions for `dev_rcHashEntries` and `dev_rcResolved`.

5. Remove old RC resources:
   - Delete `dev_rcHashEntries`, `dev_rcAccumulation`, `dev_rcResolved`, `dev_rcStub`.
   - Delete `rcComputeRootSig`, `rcEvictPso`, `rcResolvePso`, `rcUpdateRootSig`,
     `rcUpdatePso`, `dev_rcUpdateShaderIds`.
   - Delete the `RcComputeParam` and `RcUpdateParam` enums.
   - Delete `initRadianceCache()`.

6. Remove old RC params:
   - Remove `RcParams` from `common_params.h` if it's been fully replaced by `NrcConstants`.
   - Remove `rcParams` from `param_block_manager`.
   - Remove RC-specific settings (`rcCascadeScale`, `rcMinSamplesForQuery`) from
     `settings_manager.cpp`.

7. Disable path splitting when NRC is on:
   - In the UI or in the per-frame logic, force `doPathSplitting = false` when `rcEnabled`
     is true. Add a note or grayed-out UI state so it's clear why.

### Verification

- With NRC enabled, the scene should render with noticeably less noise than the plain path
  tracer at the same sample count/bounce depth, especially for indirect lighting.
- Toggle NRC on/off and compare: NRC-on should have smoother indirect illumination but
  may show some bias (over-smoothing in corners, slight color shifts). This is expected.
- Use `NrcResolveMode::DirectCacheView` (via a debug UI dropdown or hardcoded temporarily)
  to visualize the cache directly. It should show a low-detail but recognizable version of
  the scene with shadows and specular highlights.
- Use `NrcResolveMode::TrainingBounceHeatMap` to verify training paths have sensible
  lengths (more bounces in corners, fewer on open surfaces).
- Check for energy conservation: the NRC image should not be systematically brighter or
  darker than the non-NRC accumulated reference. Adjust
  `frameSettings.maxExpectedAverageRadianceValue` if needed.
- No validation layer errors, no GPU hangs on toggle.

---

## Step 6: Debug views and UI polish

**Goal**: The NRC debug resolve modes are accessible from the UI, the old RC debug view is
removed, and the toggle is polished.

### Work

1. Replace the old RC debug view (which visualized hash entries / resolved radiance) with
   NRC resolve mode selection. Add an ImGui combo box using
   `nrc::GetImGuiResolveModeComboString()` that sets `frameSettings.resolveMode`.

2. Remove old RC debug view code from `debug_view.ps.hlsl` (the `RC_HASH_ENTRIES` and
   `RC_RESOLVED` SRV bindings and any visualization logic).

3. Remove the `dev_rcStub` binding from the debug view pass.

4. Add NRC-specific settings to the UI:
   - `terminationHeuristicThreshold` slider (controls bias vs noise tradeoff).
   - `maxExpectedAverageRadianceValue` slider.
   - `learningRate` slider (advanced, optional).
   - `trainTheCache` checkbox (useful for debugging -- freezes the cache).

5. Ensure accumulation reset fires when NRC is toggled or NRC settings change.

### Verification

- All NRC resolve modes are selectable from the UI and produce the expected visualizations
  (refer to the table in the NRC guide).
- No leftover references to the old RC debug view.
- Toggling NRC on/off resets accumulation and produces correct results in both states.

---

## Step 7: Cleanup and knowledgebase update

**Goal**: Dead code is removed, the knowledgebase reflects the new architecture.

### Work

1. Delete `radiance_cache.hlsli`.
2. Remove all `RC_TABLE_SIZE`, `RC_WORKGROUP_SIZE`, `RC_UPDATE_SCALE` defines from
   `common_settings.h`.
3. Remove any remaining `#ifdef RC_UPDATE` blocks from `path_tracing.rgs.hlsl` that are
   now handled by `NrcIsUpdateMode()` / `NrcIsQueryMode()`.
4. Update `knowledge/shaders/radiance_cache.md` -> rename/rewrite to describe NRC.
5. Update `knowledge/rendering/render_passes.md` to reflect the new pass sequence.
6. Update `knowledge/shaders/path_tracing.md` to describe the NRC shader integration.
7. Update `knowledge/shaders/index.md` if shader file names changed.

### Verification

- `grep -r "rcHash\|rcAccum\|rcResolved\|rcInsert\|rcLookup\|rcWrite\|RC_UPDATE\|rc_evict\|rc_resolve\|radiance_cache\.hlsli"` finds no hits in `src/`.
- The knowledgebase accurately describes the current state.
- The app builds and runs cleanly in both NRC-on and NRC-off states.
