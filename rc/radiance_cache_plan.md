# Radiance Cache Implementation Plan

## Context

Add a spatially hashed radiance cache to the path tracer. The cache stores per-voxel irradiance in a hash table, is updated by a sparse training pass each frame, temporally blended via EMA, and queried by the main render pass for early diffuse path termination. This reduces noise at later bounces by replacing expensive path tracing with cached values when the ray cone is wide enough.

## Critical Files

- [renderer.cpp](src/rendering/renderer.cpp) - Buffer creation, root sigs, PSOs, render loop, ImGui, destroy
- [common_registers.h](src/rendering/common/common_registers.h) - Register/space assignments
- [common_params.h](src/rendering/common/common_params.h) - GPU param structs (add RC params here)
- [common_settings.h](src/rendering/common/common_settings.h) - Shared constants (add RC_TABLE_SIZE etc.)
- [global_params.hlsli](src/shaders/global_params.hlsli) - Shader-side cbuffer layout
- [path_tracing.rgs.hlsl](src/shaders/path_tracing.rgs.hlsl) - Path tracing bounce loop
- [buffer_helper.h](src/rendering/buffer/buffer_helper.h) - `createBasicBuffer`, `uavBarrier`
- [settings_manager.cpp](src/settings_manager.cpp) - `ADD_OPTION` for new settings
- [settings_gui_helpers.h](src/settings_gui_helpers.h) - ImGui wrapper helpers
- [param_block_manager.h](src/rendering/param_block_manager.h) - Param block memory layout
- [CMakeLists.txt](CMakeLists.txt) - Shader compilation (auto-detects .cs.hlsl and .rgs.hlsl)

## Existing Patterns to Reuse

- `BufferHelper::createBasicBuffer(size, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS })` for UAV buffers
- `BufferHelper::uavBarrier(cmdList, resource)` for UAV-to-UAV sync
- `BufferHelper::stateTransitionResourceBarrier(cmdList, resource, before, after)` for state transitions
- `MAKE_PARAM(type, regPrefix, name)` macro for root signature params
- `Util::caclulateDispatchSize(size, threadGroupSize)` for compute dispatch sizing
- `makeShaderBytecode(bytecodeArray)` for compute PSO creation
- `SettingsGuiHelpers::Checkbox/SliderFloat/SliderUint` for ImGui controls
- `ADD_OPTION(name, desc, type, defaultValue)` for settings registration
- Collect PSO pattern: `D3D12_COMPUTE_PIPELINE_STATE_DESC` + `CreateComputePipelineState`

---

## Buffer Layout Reference

The cache uses three parallel arrays, all indexed by hash slot:

**`dev_rcHashEntries`** — `RWByteAddressBuffer`, 8 bytes per slot. Key storage for collision detection. When a position is hashed to a slot, the stored key is compared against the expected key to confirm it's a real match and not a collision. Each entry stores a packed 64-bit representation of the `int3` grid coordinate via `rcPackKey`:
- first uint32: 24 bits of `gridPos.x` | low 8 bits of `gridPos.y` shifted to bits 24–31
- second uint32: high 8 bits of `gridPos.y` in bits 0–7 | 24 bits of `gridPos.z` shifted to bits 8–31

A value of `uint2(RC_EMPTY_SENTINEL, RC_EMPTY_SENTINEL)` (i.e. `0xFFFFFFFF`) means the slot is empty. Uses `RWByteAddressBuffer` (not `RWStructuredBuffer<uint2>`) so that `InterlockedCompareExchange` works for atomic slot insertion.

**`dev_rcAccumulation`** — `RWByteAddressBuffer`, 16 bytes per slot. Per-frame scratch buffer that collects new radiance samples via atomic adds. Zeroed at the start of every frame by the eviction pass and written to only by the update pass:
- `.x` (uint32): accumulated red, stored as fixed-point (`radiance.r * RC_RADIANCE_SCALE`, cast to uint)
- `.y` (uint32): accumulated green, same encoding
- `.z` (uint32): accumulated blue, same encoding
- `.w` (uint32): accumulated sample count (each sample adds `RC_SAMPLE_MULTIPLIER` for sub-sample precision)

Uses uints because `InterlockedAdd` works on integers but not floats in HLSL. Multiple threads from the update pass can write to the same cell simultaneously and the atomics ensure the values sum correctly. The resolve pass reads this, divides out the scale factors to recover the average radiance for the frame, then blends it into the resolved buffer.

**`dev_rcResolved`** — one `float4` (16 bytes) per slot. Persistent, temporally blended radiance estimate that the main render pass reads from:
- `.x` (float): resolved red radiance (exponential moving average across frames)
- `.y` (float): resolved green radiance
- `.z` (float): resolved blue radiance
- `.w` (float): total accumulated sample weight — used for the EMA blend factor calculation, and also serves as a quality indicator (the render pass checks if this exceeds `rcMinSamplesForQuery` before using the cached value)

This buffer is never zeroed wholesale — it persists across frames and represents the cache's current best estimate. Individual entries get cleared only when the eviction pass determines they're stale. The resolve pass updates it each frame by blending in the new accumulation data. The `.w` weight decays each frame via `previousWeight * RC_DECAY`, so entries that stop receiving samples gradually lose confidence and eventually get evicted.

---

## Step 1: Add RC Constants, Params, and Settings

**Goal:** Define all shared constants and GPU parameters so they compile and are accessible from both C++ and HLSL. No GPU work yet.

### Tasks:
1. In [common_settings.h](src/rendering/common/common_settings.h), add:
   - `#define RC_TABLE_SIZE (1u << 22)` (4M entries)
   - `#define RC_WORKGROUP_SIZE 256`
   - `#define RC_STALE_WEIGHT_THRESHOLD 0.1` (entries with `.w` below this are evicted)
   - `#define RC_RADIANCE_SCALE 1024.0`
   - `#define RC_SAMPLE_MULTIPLIER 1024`
   - `#define RC_DECAY 0.97`
   - `#define RC_UPDATE_SCALE 5` (update pass runs at 1/5 resolution)

2. In [common_params.h](src/rendering/common/common_params.h), add a new `RadianceCacheParams` struct (16-byte aligned) with:
   - `uint rcFrameNumber`
   - `float rcVoxelSize`
   - `uint rcEnabled`
   - `uint rcMinSamplesForQuery`

3. In [global_params.hlsli](src/shaders/global_params.hlsli), add `RadianceCacheParams rcParams;` to the cbuffer.

4. In [param_block_manager.h/cpp](src/rendering/param_block_manager.h), add `RadianceCacheParams* rcParams` pointer and set it up in `init()` following the existing pointer arithmetic pattern.

5. In [settings_manager.cpp](src/settings_manager.cpp), add settings:
   - `rcEnabled` (bool, default "true")
   - `rcVoxelSize` (float, default "1.0")
   - `rcDecay` (float, default "0.97")
   - `rcMinSamplesForQuery` (uint32_t, default "4")

6. In [renderer.cpp](src/rendering/renderer.cpp) `beginFrame()`, sync RC settings to `rcParams`:
   - `rcParams->rcFrameNumber = rcFrameNumber++` (new static counter that never resets)
   - `rcParams->rcVoxelSize = SettingsManager::getAsFloat("rcVoxelSize")`
   - `rcParams->rcEnabled = SettingsManager::getAsBool("rcEnabled") && voxelMode ? 1 : 0`
   - `rcParams->rcMinSamplesForQuery = SettingsManager::getAsUint("rcMinSamplesForQuery")`

7. In the ImGui section of `renderer.cpp`, add a new "Radiance Cache" section (after "Materials", before "Antialiasing") with:
   - `rcEnabled` checkbox
   - `rcVoxelSize` slider (0.25 to 4.0)
   - `rcDecay` slider (0.9 to 0.999)
   - `rcMinSamplesForQuery` slider (1 to 32)

**Verify:** Project compiles. ImGui controls appear and update settings. GPU param struct alignment is correct (static_assert).

---

## Step 2: Create GPU Buffers

**Goal:** Allocate the three RC hash table buffers on the GPU. No shaders yet.

### Tasks:
1. In [renderer.cpp](src/rendering/renderer.cpp), add static buffer declarations (near `dev_gbuffer`):
   - `static ComPtr<ID3D12Resource> dev_rcHashEntries;` (uint2 per entry = 8 bytes each)
   - `static ComPtr<ID3D12Resource> dev_rcAccumulation;` (uint4 per entry = 16 bytes each)
   - `static ComPtr<ID3D12Resource> dev_rcResolved;` (float4 per entry = 16 bytes each)

2. Create them in `init()`, after `initRootSignature()` / before `initPipeline()` (or in a dedicated `initRadianceCache()` called from `init()`). Use `createBasicBuffer` with UAV flag. Set names.

3. In `destroy()`, add `.Reset()` for all three buffers (near `dev_gbuffer.Reset()`).

**Verify:** Project compiles and runs. Check GPU memory usage increased by ~160MB. No crashes on startup/shutdown.

---

## Step 3: Eviction/Clear Compute Shader + Pipeline

**Goal:** Create the first compute pass that clears stale entries and zeros the accumulation buffer each frame.

### Tasks:
1. Add register/space definitions in [common_registers.h](src/rendering/common/common_registers.h):
   - `#define RC_REGISTER_SPACE 3` (or next available space)
   - `#define RC_REGISTER_HASH_ENTRIES 0` (u0)
   - `#define RC_REGISTER_ACCUMULATION 1` (u1)
   - `#define RC_REGISTER_RESOLVED 2` (u2)

2. Create `RcEvictParam` enum and `RC_EVICT_PARAM_IDX` macro in [renderer.cpp](src/rendering/renderer.cpp):
   ```
   enum class RcEvictParam { GLOBAL_PARAMS, HASH_ENTRIES, ACCUMULATION, RESOLVED, COUNT };
   ```

3. Build `rcEvictRootSig` in `initRootSignature()`:
   - GLOBAL_PARAMS (CBV)
   - HASH_ENTRIES (UAV, RC space)
   - ACCUMULATION (UAV, RC space)
   - RESOLVED (UAV, RC space)

4. Create shader file [rc_evict.cs.hlsl](src/shaders/rc_evict.cs.hlsl):
   - Include `global_params.hlsli` and `common_settings.h`
   - Declare the three UAV buffers with RC register macros
   - Thread per entry: if key != 0, check `dev_rcResolved[slot].w` (the accumulated sample weight). If it has decayed below a staleness threshold (e.g., close to zero), the entry has stopped receiving samples and is stale — zero all three buffers at that slot to free it for reuse.
   - Unconditionally zero `dev_rcAccumulation[slot]` (this is the per-frame scratch buffer that must be clean before the update pass writes to it).

5. Create `rcEvictPso` in `initPipeline()` following the collect PSO pattern.

6. In the render loop, between the GBuffer barrier and the path tracing section, add the eviction dispatch:
   - Set pipeline + root sig
   - Bind GLOBAL_PARAMS, hash entries, accumulation, resolved as UAVs
   - Dispatch `ceil(RC_TABLE_SIZE / RC_WORKGROUP_SIZE)` groups

7. Add UAV barriers after the dispatch for all three buffers (or a single `nullptr` UAV barrier for simplicity).

8. Add `.Reset()` calls in `destroy()` for `rcEvictPso` and `rcEvictRootSig`.

**Verify:** Project compiles and runs. No visual change (eviction pass runs but does nothing visible). Use PIX/NSight to confirm the compute dispatch executes with correct thread counts.

---

## Step 4: Resolve Compute Shader + Pipeline

**Goal:** Add the resolve pass that blends per-frame accumulation into the persistent resolved buffer.

### Tasks:
1. Create `RcResolveParam` enum (same layout as evict — reuse the same root sig, or make a separate one if cleaner).

2. Create shader file [rc_resolve.cs.hlsl](src/shaders/rc_resolve.cs.hlsl):
   - Thread per entry. If key != 0 and `accumulation[slot].w > 0`:
     - Compute `currentRadiance` from accumulation: divide `.xyz` by (`.w / RC_SAMPLE_MULTIPLIER`) to get average radiance, then divide by `RC_RADIANCE_SCALE` to undo the fixed-point encoding
     - Read `previousRadiance` from `resolved[slot].rgb` and `previousWeight` from `resolved[slot].w`
     - Decay the previous weight: `previousWeight *= RC_DECAY`
     - Compute current sample count: `currentSamples = accumulation[slot].w / RC_SAMPLE_MULTIPLIER`
     - Blend with EMA: `newWeight = previousWeight + currentSamples`, `blendFactor = currentSamples / newWeight`, `newRadiance = lerp(previousRadiance, currentRadiance, blendFactor)`
     - Write `resolved[slot] = float4(newRadiance, newWeight)`
   - If key != 0 but `accumulation[slot].w == 0` (entry exists but received no new samples this frame):
     - Decay the weight: `resolved[slot].w *= RC_DECAY`
     - Leave `.xyz` unchanged
   - The eviction pass will clear entries whose `.w` has decayed below a staleness threshold

3. Create `rcResolvePso` in `initPipeline()`.

4. In the render loop, dispatch after the eviction pass (with a UAV barrier in between):
   - Same binding pattern as eviction
   - Same dispatch dimensions

5. Add a UAV barrier after resolve (the resolved buffer will be read as SRV by the main path tracer later, but for now just barrier).

6. Add `.Reset()` calls in `destroy()`.

**Verify:** Project compiles and runs. Still no visual change. Confirm in PIX/NSight that both compute dispatches execute.

---

## Step 5: Radiance Cache Utility Header (HLSL)

**Goal:** Create the shared HLSL functions for hash grid operations, used by both the update pass and the render pass.

### Tasks:
1. Create [radiance_cache.hlsli](src/shaders/radiance_cache.hlsli) with:
   - `rcWorldToGrid(float3 pos_WS, float voxelSize)` → `int3`
   - `rcSpatialHash(int3 gridPos)` → `uint` (Teschner + Wang finalizer, masked to `RC_TABLE_SIZE - 1`)
   - `rcPackKey(int3 gridPos)` → `uint2`
   - `rcInsertOrFind(int3 gridPos, RWByteAddressBuffer hashEntries)` → `uint` slot or `~0u`
     - Use `RWByteAddressBuffer` with `InterlockedCompareExchange` for atomic CAS on the first uint of each entry. This is more reliable than `RWStructuredBuffer<uint2>` for atomics.
   - `rcLookup(int3 gridPos, ByteAddressBuffer hashEntries)` → `uint` (read-only version, no insertion)
   - `rcJitterPos(float3 pos_WS, float voxelSize, inout RandomNumberGenerator rng)` → `float3` — offsets position by `(rng.nextFloat3() - 0.5f) * RC_JITTER_SCALE * voxelSize` to blur voxel boundaries when querying
     - `RC_JITTER_SCALE` (0.1f) and `RC_EMPTY_SENTINEL` (0xFFFFFFFFu) added as constants in `common_settings.h`
   - `rcWriteRadiance(uint slot, float3 radiance, RWByteAddressBuffer accumBuffer)` — atomic adds of quantized radiance + sample count

Hash function:

```
uint rcSpatialHash(int3 gridPos)
{
    uint h = (uint)gridPos.x * 73856093u
           ^ (uint)gridPos.y * 19349663u
           ^ (uint)gridPos.z * 83492791u;
    h = (h ^ 61u) ^ (h >> 16u);
    h *= 9u;
    h ^= h >> 4u;
    h *= 0x27d4eb2du;
    h ^= h >> 15u;
    return h & (RC_TABLE_SIZE - 1u);
}
```

2. If using `RWByteAddressBuffer` for hash entries: update the buffer creation in step 2 if needed (byte address buffers may need `D3D12_BUFFER_UAV_FLAG_RAW` or simply be bound differently). Also update the evict/resolve shaders to use `RWByteAddressBuffer` for hash entries instead of `RWStructuredBuffer<uint2>`.

**Verify:** Project compiles. No runtime change yet — functions are defined but not called.

---

## Step 6: RC Update Pass (Cache Training)

**Goal:** Add the sparse update dispatch that traces paths at reduced resolution and writes radiance into the cache.

### Tasks:
1. Add new registers in [common_registers.h](src/rendering/common/common_registers.h) for the RC update variant of the path tracer. The update pass needs the hash entries and accumulation buffers as UAVs. Since the path tracing root sig uses space 2 for PT-specific resources, add RC UAV registers in a new space (e.g., `RC_PT_REGISTER_SPACE 3`):
   - `RC_PT_REGISTER_HASH_ENTRIES 0` (u0 in RC_PT space)
   - `RC_PT_REGISTER_ACCUMULATION 1` (u1 in RC_PT space)

2. Create `RcUpdateParam` enum in [renderer.cpp](src/rendering/renderer.cpp) — same as `PtParam` but with two additional UAV entries for the RC buffers.

3. Build `rcUpdateRootSig` in `initRootSignature()` — copy the PT root sig params and add the two RC UAV entries.

4. Create [rc_update.rgs.hlsl](src/shaders/rc_update.rgs.hlsl):
   ```hlsl
   #define RC_UPDATE 1
   #include "path_tracing.rgs.hlsl"
   ```

5. Modify [path_tracing.rgs.hlsl](src/shaders/path_tracing.rgs.hlsl):
   - `#ifdef RC_UPDATE`: declare `RWByteAddressBuffer` for hash entries and accumulation with RC_PT register macros
   - `#ifdef RC_UPDATE`: include `radiance_cache.hlsli`
   - In `RayGeneration()`: when `RC_UPDATE` is defined, compute dispatch pixel differently — each thread covers a 5x5 tile, picks a random pixel within it using the RNG
   - Disable path splitting for the update pass
   - **Simple first approach for writing:** At the end of the path (when returning from the bounce loop), if the last surface was diffuse, write the accumulated `pathColor` at that vertex's grid position into the cache. This is simpler than backward accumulation and still populates the cache.

6. Create `rcUpdatePso` (RT PSO) in `initPipeline()` — same as `ptPso` but using `rc_update_rgs_shaderBytecode` and `rcUpdateRootSig`.

7. In the render loop, dispatch the update pass between the eviction/resolve and the main path tracing pass:
   - Set `rcUpdatePso` + `rcUpdateRootSig`
   - Bind all RT resources (same as PT) plus RC UAVs
   - DispatchRays at `ceil(renderWidth / RC_UPDATE_SCALE) x ceil(renderHeight / RC_UPDATE_SCALE)`
   - UAV barriers on hash entries and accumulation after the dispatch

8. Move the resolve dispatch to AFTER the update pass (since resolve needs to read what the update pass wrote).

**Revised render loop order:**
```
GBuffer → RC Evict → barrier → RC Update → barrier → RC Resolve → state transition → Main PT → ...
```

RC Evict should not have any dependency on gbuffer data, but double check this to be sure.

**Verify:** Project compiles and runs. Add temporary debug output: in the evict or resolve shader, count non-zero entries and write to a debug buffer, or use the debug visualization (next step) to confirm entries are being populated. The rendered image should look the same (main PT hasn't changed yet).

---

## Step 7: Debug Visualization

**Goal:** Add debug views to visually verify the cache is being populated correctly.

### Tasks:
1. Add two debug view options. Since the existing debug view system maps strings to `RtTarget*`, and these RC visualizations need to be written from the path tracing shader, use the existing `debugTarget` with a mode selector:
   - Add `rcDebugView` setting (uint, 0=off, 1=grid cells, 2=cached radiance)
   - Add `rcDebugView` to `RenderParams` or `DebugParams`
   - Sync in `beginFrame()`

2. In the main path tracing shader (`#ifndef RC_UPDATE` path), at the first diffuse hit (pathDepth == 0, non-delta surface):
   - If `rcDebugView == 1`: compute grid cell hash, derive a random color, write to `debugTexture()`, return
   - If `rcDebugView == 2`: look up resolved cache value, write to `debugTexture()`, return

3. Add ImGui combo for the RC debug view in the Radiance Cache settings section.

**Verify:** Select "RC Grid Cells" debug view — see a colored voxel grid overlay. Select "RC Cached Radiance" — see the cached irradiance (should show rough lighting after a few frames of the update pass running). This confirms the hash grid is working and entries are being populated correctly.

---

## Step 8: Cache Read in Main Render Pass

**Goal:** The main path tracer reads the cache for early termination at diffuse bounces.

NOTE: this may be out of date since I did some stuff from step 9 before doing this step.

### Tasks:
1. Add RC resolved buffer + hash entries as SRVs to the main PT root signature. Add new register definitions for reading:
   - In `common_registers.h`, add `PT_REGISTER_RC_RESOLVED` and `PT_REGISTER_RC_HASH_ENTRIES` as `t` registers in `PT_REGISTER_SPACE`
   - Add to `PtParam` enum
   - Add to PT root sig in `initRootSignature()`

2. In [path_tracing.rgs.hlsl](src/shaders/path_tracing.rgs.hlsl), in the `#ifndef RC_UPDATE` path:
   - Declare `ByteAddressBuffer` for hash entries and `StructuredBuffer<float4>` for resolved, using the PT register macros
   - Include `radiance_cache.hlsli`
   - In the bounce loop, after `TraceRay` and ray cone update, at `pathDepth >= 1` when the surface is diffuse and `rcParams.rcEnabled`:
     - Check `payload.rayCone.width >= rcParams.rcVoxelSize`
     - **Jitter the query position** to blur voxel boundaries and break up structured noise: offset the hit position by a random vector within `[-0.5, 0.5] * rcVoxelSize` per axis (using the existing RNG) before computing the grid cell. This smooths transitions between adjacent cache cells.
     - `rcLookup` the jittered grid cell
     - If found and `resolved.w >= rcParams.rcMinSamplesForQuery` (sufficient accumulated sample weight): `pathColor += payload.pathWeight * resolved.rgb; return;`

3. In the render loop, bind the resolved buffer and hash entries as SRVs when setting up the main PT dispatch. Add a state transition for `dev_rcResolved` from UAV to `NON_PIXEL_SHADER_RESOURCE` before the main PT dispatch, and transition back to UAV state before the next frame's eviction pass (or rely on implicit promotion/decay since the buffer starts in COMMON state).

**Verify:** Enable the radiance cache. The image should render with reduced noise at later bounces. Compare with RC disabled — there may be some bias initially (expected). Toggle RC on/off to confirm it's working. Check that early bounces (pathDepth 0-1) still trace normally and only later diffuse bounces terminate into the cache.

---

## Step 9: Tuning and Polish

**Goal:** Refine parameters and fix artifacts.

### Tasks:
- ~~Test different `rcVoxelSize` values — too small causes hash collisions, too large causes light leaking.~~
- ~~Add firefly clamping in the resolve pass if needed.~~
- ~~Reproject grid cells when camera moves or global instance offset changes~~
- ~~Cascading voxel size, roughly proportional to footprint on screen (closer to camera or more zoomed in = finer cell size)~~
- ~~Ensure that jitter is only being used for querying the cache, not for writing to it (just to be absolutely sure)~~
- ~~Use a jitter pattern instead of uniform RNG for shooting rays in the RC update pass, for better coverage of the entire screen.~~
- Adjust `RC_DECAY` for temporal stability vs. responsiveness.
- Adjust `rcMinSamplesForQuery` — too low causes noise from undersampled cache entries.
- Consider whether the `RC_UPDATE_SCALE` of 5 gives enough training coverage.
- Profile the frame time impact of the three new passes.
- Deallocate the RC buffers (`dev_rcHashEntries`, `dev_rcAccumulation`, `dev_rcResolved`) when the radiance cache is disabled to free ~160MB -VRAM. Reallocate them when the radiance cache is re-enabled.
- Add contribution from all paths starting from diffuse, not just the first one
- Increase RC update path depth beyond main PT pass path depth? i.e. dd a few max bounces (maybe 2?) in RC_UPDATE mode, since main PT pass -probably only use a few total bounces
- Refactor debug view logic so it's not so convoluted - separate it into a different pass, maybe try to bind less stuff
- Ensure that cache is being written to for hits under the ocean when camera is above ocean (maybe even consider enabling path splitting for RC update pass)
- Maybe jitter across cascade levels?

**Verify:** Visually compare cached vs. uncached renders for quality. Check frame time impact. Ensure no light leaking at voxel boundaries.

---

## Verification Summary

| Step | What to check |
|------|--------------|
| 1 | Compiles, ImGui controls appear, settings round-trip |
| 2 | Compiles, runs, ~160MB more VRAM used |
| 3 | Compiles, eviction dispatch visible in PIX/NSight |
| 4 | Compiles, resolve dispatch visible in PIX/NSight |
| 5 | Compiles (no runtime change) |
| 6 | Compiles, cache entries populated (verify via debug viz or PIX) |
| 7 | Debug views show grid cells and cached radiance |
| 8 | Main render uses cache, reduced noise at later bounces |
| 9 | Visual quality and performance acceptable |
