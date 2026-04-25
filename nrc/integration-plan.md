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

- **Resolve approach**: Use a custom resolve compute shader instead of NRC's built-in
  `Resolve()`. The built-in resolve almost certainly expects a 2D texture as its output
  buffer — the Vulkan API makes this explicit (`VkImageView`), and the D3D12 version
  likely creates a `RWTexture2D` UAV internally despite accepting a generic
  `ID3D12Resource*`. Our path tracing output (`dev_pathTracingRawBuffer`) is a structured
  buffer, not a texture, so the built-in resolve would fail or produce incorrect results.

  The custom resolve is straightforward (~15 lines of HLSL — the NRC guide provides an
  example in its Step 7). It reads NRC's `QueryPathInfo` and `QueryRadiance` buffers,
  multiplies by the path's prefix throughput, and adds the result to
  `dev_pathTracingRawBuffer` at the correct buffer index.

  The custom resolve also supports path splitting. When path splitting is active, each
  split thread creates its own NRC query at the doubled dispatch resolution. The
  interleaved buffer layout (`linearPixelIdx * 2 + pathSplitIdx`) is numerically
  equivalent to linear indexing at the doubled width (`dy * renderWidth*2 + dx`), so the
  custom resolve just indexes linearly using the doubled `frameDimensions`. No additional
  buffer infrastructure changes are needed.

- **Path splitting + NRC**: Path splitting remains active when NRC is enabled. NRC's
  `frameDimensions` is set to `(renderWidth * 2, renderHeight)` when `doPathSplitting` is
  on, matching the doubled dispatch width. Each split thread traces its own BSDF lobe and
  creates its own NRC query point; the custom resolve writes each result to the correct
  interleaved slot. The NRC update pass is unaffected — it always runs at
  `trainingDimensions` without path splitting.

  This doubles NRC query buffer memory compared to non-split mode (NRC allocates based on
  `frameDimensions`), but the cost is acceptable given the signal quality benefit of path
  splitting. `configureNrc()` must read `doPathSplitting` and reconfigure when it changes.

  For debug visualizations (heatmaps, direct cache view, etc.), use NRC's built-in
  `Resolve()` with a temporary `RWTexture2D<float4>` debug texture and display it through
  the existing debug view pipeline.

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
     - `frameDimensions` = `(renderWidth * (doPathSplitting ? 2 : 1), renderHeight)`.
       When path splitting is on, the query dispatch width is doubled, so
       `frameDimensions` must match.
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

6. Handle reconfiguration: if the render resolution or `doPathSplitting` changes while NRC
   is active, call `nrcContext->Configure(newContextSettings)` again (since both affect
   `frameDimensions`). Resolution changes can be handled in the resize() method;
   `doPathSplitting` changes also trigger a resize of `dev_pathTracingRawBuffer`, so the
   same code path works.

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

- **Startup double-init guarded.** `initNrc()` now returns early if a context already
  exists, and the per-frame `nrcPrevEnabled` state is initialized from the current
  `nrcEnabled` setting. This prevents calling `Initialize`/`Create` twice when NRC starts
  enabled from CLI or persisted settings.

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

## Step 4: Shader-side NRC integration (update and query variants) — DONE

**Goal**: The path tracing shader is modified to use NRC's shader API. Three shader
variants are compiled. NRC buffers are bound and written to.

### Work

1. **Create new shader entry files**:
   - `nrc_update.rgs.hlsl`: `#define NRC_UPDATE 1` then `#include "path_tracing.rgs.hlsl"`.
   - `nrc_query.rgs.hlsl`: `#define NRC_QUERY 1` then `#include "path_tracing.rgs.hlsl"`.
   - Keep `rc_update.rgs.hlsl` for now because the old RC path still coexists with NRC
     during the migration. Old RC shader deletion is deferred to Step 5/Step 7 cleanup.

2. **Modify `path_tracing.rgs.hlsl`**:
   - Add NRC includes for the NRC variants while keeping `radiance_cache.hlsli` for the
     non-NRC / old-RC variants:
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
       NrcContext nrcCtx = NrcCreateContext(nrcConstants, nrcBuffers, nrcPixelIdx);
       NrcPathState nrcPathState = NrcCreatePathState(nrcConstants, rng.nextFloat());
       ```
      `nrcPixelIdx` is `DispatchRaysIndex().xy`, not `getPixelIdx()`, because NRC
      `frameDimensions` match the full dispatch dimensions. With path splitting enabled,
      `getPixelIdx()` intentionally divides X by two for normal shading, but NRC needs the
      unique doubled-width path coordinate.
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

4. **Defer old RC shader deletion**: `rc_evict.cs.hlsl`, `rc_resolve.cs.hlsl`,
   `rc_update.rgs.hlsl`, and `radiance_cache.hlsli` remain while both cache
   implementations coexist. Their deletion is tracked in Step 5/Step 7.

5. **Update `shaders.cpp`**: Add the new NRC update/query shader registrations while
   retaining the old RC shader registrations for coexistence.

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

### Verification status

- Project builds in `RelWithDebInfo`.
- Enabling NRC no longer causes the first-frame GPU hang that occurred during Step 4
  bring-up.
- The crash root cause was dispatching `nrcQueryPso` with the plain path tracing shader
  table (`ptDispatchDesc`). The render loop now selects the active dispatch descriptor:
  `nrcQueryDispatchDesc` for `nrcQueryPso`, `ptDispatchDesc` for `ptPso`.
- NRC path coordinates now use raw `DispatchRaysIndex().xy` for `NrcCreateContext()`, so
  query/update records match NRC `frameDimensions`, including doubled-width path-splitting
  dispatches.
- Early NRC shader exits were tightened so created NRC path states reach
  `NrcWriteFinalPathInfo()` before leaving the shader.
- Mixed diffuse+glossy materials now report the actual traced lobe to NRC. The query pass
  applies first-bounce path splitting before `NrcUpdateOnHit()`, and after BSDF sampling it
  updates NRC's previous-hit delta flag from `surfBsdfSample.wasSpecular`. This prevents
  specular reflection paths from being treated as non-delta and querying NRC too early.

### Deviations from plan

- **Old RC is still present.** Step 4 originally called for deleting old RC shader files
  and registrations, but the branch currently keeps `rcEnabled` and `nrcEnabled` separate
  so both implementations can coexist during migration. Deletion remains part of the Step
  5/Step 7 cleanup.
- **NRC query uses a separate dispatch descriptor.** The initial Step 4 implementation
  created a query PSO and shader table but accidentally dispatched it with `ptDispatchDesc`.
  This was fixed by using `nrcQueryDispatchDesc` whenever `nrcQueryPso` is active.
- **NRC indexing is separate from shading pixel indexing.** Normal path tracing still uses
  `getPixelIdx()` for gbuffer/shading semantics, but NRC context creation uses the raw
  dispatch index to avoid path-split coordinate aliasing.

---

## Step 5: QueryAndTrain and Resolve passes -- done

**Goal**: The NRC neural network is trained and queried each frame, and the predicted
radiance is resolved into the output buffer. The image should look correct with NRC
enabled.

### Work

1. In the render loop, after the query dispatch (which replaces the old path tracing
   dispatch), call:
   ```cpp
   nrcContext->QueryAndTrain(cmdList, nullptr); // nullptr = don't track training loss
   ```

2. Create a custom resolve compute shader (`nrc_resolve.cs.hlsl`):
   - NRC's built-in `Resolve()` expects a 2D texture output, but our
     `dev_pathTracingRawBuffer` is a structured buffer. A custom resolve writes to it
     directly.
   - The shader dispatches at `frameDimensions` (which equals the query dispatch size —
     `renderWidth * (doPathSplitting ? 2 : 1)` by `renderHeight`). For each dispatch
     thread:
     1. Computes `pathIndex` via `NrcGetPathInfoIndex(frameDimensions, dispatchIdx, 0, 1)`.
     2. Unpacks the `NrcQueryPathInfo` from the `QueryPathInfo` buffer.
     3. If `queryBufferIndex < 0xFFFFFFFF`, unpacks the predicted radiance from the
        `QueryRadiance` buffer, multiplies by `prefixThroughput`, and adds to
        `dev_pathTracingRawBuffer[bufferIdx]` where
        `bufferIdx = dispatchIdx.y * frameDimensions.x + dispatchIdx.x`. This linear
        index matches the interleaved path-split layout because
        `(pixelY * renderWidth + pixelX) * 2 + splitIdx` equals
        `pixelY * (renderWidth * 2) + (pixelX * 2 + splitIdx)` equals
        `dispatchY * frameDimensions.x + dispatchX`.
   - The shader needs access to: `NrcConstants` (from the param block), `QueryPathInfo`
     (UAV), `QueryRadiance` (UAV), `dev_pathTracingRawBuffer` (UAV). The SDK-owned NRC
     buffers are read-only in the shader, but they stay bound as UAV root descriptors to
     match the buffers' SDK usage and current resource state.
   - Create a root signature, PSO, and dispatch for this pass.
   - Dispatch after `QueryAndTrain`, before Collect.

   Debug resolve modes are deferred to Step 6. Normal rendering uses the custom
   structured-buffer resolve.

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
- Toggle path splitting on/off with NRC active — both produce correct images, no crash.
  With path splitting on, the NRC query buffer count should be roughly double.
- No validation layer errors, no GPU hangs on toggle.

### Verification status

- Project builds in `RelWithDebInfo`.
- Runtime check: enabling NRC renders correctly, and resizing with NRC enabled no longer
  crashes.
- The render loop now runs:
  `BeginFrame -> G-buffer -> NRC update -> NRC query -> QueryAndTrain -> restore app descriptor heap -> custom resolve -> Collect -> EndFrame`.
- `QueryAndTrain()` internally binds the NRC descriptor heap. The app descriptor heap is
  rebound immediately afterward so the custom resolve, Collect, and later passes index the
  expected `ResourceDescriptorHeap`.
- The custom resolve writes directly into `dev_pathTracingRawBuffer`; raw PT buffers are
  transitioned back to UAV after Collect so following frames do not depend on implicit
  buffer state decay.
- Old RC runtime passes/resources/settings have been removed from the active renderer path.

### Deviations from plan

- **Debug resolve modes moved to Step 6.** The temporary debug texture and built-in
  `nrcContext->Resolve()` path are UI/debug tooling, not required for normal Step 5
  rendering. They remain tracked under Step 6.
- **NRC query buffers are bound as UAVs in custom resolve.** Although the resolve shader
  only reads `QueryPathInfo` and `QueryRadiance`, they are SDK-owned buffers used through
  UAV root descriptors elsewhere in the frame. Keeping the custom resolve bindings as UAVs
  avoids mismatched resource/descriptor expectations.

---

## Step 6: Debug views and UI polish — done

**Goal**: The NRC debug resolve modes are accessible from the UI, the old RC debug view is
removed, and the toggle is polished.

### Work

1. Replace the old RC debug view (which visualized hash entries / resolved radiance) with
   NRC resolve mode selection. Add an ImGui combo box using
   `nrc::GetImGuiResolveModeComboString()` that sets `frameSettings.resolveMode`.
   When a debug resolve mode is selected, use the built-in `Resolve()` with the debug
   texture instead of the custom resolve, and display the debug texture through the debug
   view pipeline.

2. Remove old RC debug view code from `debug_view.ps.hlsl` (the `RC_HASH_ENTRIES` and
   `RC_RESOLVED` SRV bindings and any visualization logic).

3. Remove the `dev_rcStub` binding from the debug view pass.

4. Add NRC-specific settings to the UI:
   - `terminationHeuristicThreshold` slider (controls bias vs noise tradeoff).
   - `trainingTerminationHeuristicThreshold` slider, or keep it paired with
     `terminationHeuristicThreshold`.
   - `maxExpectedAverageRadianceValue` slider.
   - `skipDeltaVertices` checkbox (useful when debugging mirror/specular paths).
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
