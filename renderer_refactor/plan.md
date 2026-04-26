# Renderer Refactor Plan

`src/rendering/renderer.cpp` is 2537 lines. This plan breaks the refactor into
two phases: a mechanical file split (Phase A), then a targeted abstraction to
eliminate duplication (Phase B). Phase A has no behavioral change. Phase B
builds on A.

Read `knowledge/rendering/` before starting — especially `render_passes.md`,
`frame_contexts.md`, `param_blocks.md`, and `rt_targets.md`.

---

## Current Structure of renderer.cpp

All code lives in the `Renderer` namespace as static functions and variables.
Logical sections (with approximate line ranges as of 2026-04-26):

| Section | Lines | Description |
|---|---|---|
| Includes + macros | 1–108 | Streamline macros, `CHECK_SL_RESULT` |
| Forward decls + state | 109–160 | `FrameContext`, frame state, camera, scene |
| `init()` | 161–222 | Top-level init orchestrator |
| `loadScene()` | 224–235 | Flush + glTF load + NRC reconfigure |
| `initStreamline()` | 237–269 | Streamline SDK init |
| `initDevice()` | 271–358 | DXGI factory, adapter enum, device, command queue |
| `initDescriptorHeaps()` | 360–383 | Shared CBV/SRV/UAV heap + RTV heap |
| `initNvapi()` | 385–405 | NVAPI init, SER detection |
| `initSwapChain()` | 407–454 | Swap chain creation, tearing support |
| RtTarget declarations | 456–474 | All `RtTarget` statics |
| `initRtTargets()` | 476–498 | Push into `allRtTargets` / `autoTransitionRtTargets` |
| Raw buffers + resize state | 500–536 | `dev_gbuffer`, `dev_pathTracingRawBuffer`, DLSS state |
| `resize()` | 537–704 | Resize swap chain, recreate buffers/targets, DLSS config |
| `initCommand()` | 706–716 | Command allocators + command list |
| `initConstantParams()` | 718–730 | RNG seed |
| Param enums + macros | 732–836 | `GbufferParam`, `PtParam`, etc. + `MAKE_PARAM` macro |
| `initRootSignature()` | 838–1098 | 6 root signatures (gbuffer, PT, collect, NRC resolve, postprocess, debug view) |
| PSO statics | 1100–1121 | All PSO + shader table + dispatch desc statics |
| `initPipeline()` | 1125–1343 | 6 pipeline state objects |
| `configureNrc()` / `initNrc()` / `destroyNrc()` | 1346–1416 | NRC lifecycle |
| `initImgui()` | 1418–1446 | Dear ImGui + ImPlot init |
| FPS tracking | 1448–1464 | `updateFps()` |
| Screenshot | 1466–1589 | `queueScreenshot`, `captureQueuedScreenshot`, `finalizeQueuedScreenshot` |
| ImGui combo data | 1592–1623 | Combo option arrays + `debugViewComboMap` |
| `imguiBeginFrame()` / `imguiEndFrame()` | 1625–1789 | GUI layout and settings mutation |
| `makeSlResource()` | 1791–1800 | Streamline resource helper |
| `render()` | 1804–2390 | **Main render loop** — DLSS tagging, param fill, pass dispatch, transitions, present |
| `beginFrame()` / `submitCmd()` | 2392–2411 | Command list lifecycle |
| `flush()` / `getFrameIndex()` | 2413–2427 | Sync helpers |
| `destroy()` | 2429–2537 | Full teardown |

---

## Phase A: File Split

Move code into separate `.cpp` files. Shared state goes in `renderer_internal.h`.
No new abstractions, no behavioral change.

### renderer_internal.h

Exposes everything the split files need to see. This is not a public header — it
stays in `src/rendering/` and is only included by the `renderer_*.cpp` files.

Contents:
- `FrameContext` struct definition
- `extern` declarations for all shared statics: `device`, `fence`,
  `graphicsCmdQueue`, `cmdList`, `swapChain`, `frameCtxs[]`, `frameCtxIdx`,
  `sharedDescriptorHeap`, `sharedDescHeapAlloc`, `rtvHeap`, all `RtTarget`
  statics, all raw buffer `ComPtr`s, all root signature `ComPtr`s, all PSO
  `ComPtr`s, all dispatch descs, `scene`, `camera`, `nrcContext`,
  `viewport`/`scissor`, `renderWidth`/`renderHeight`, `voxelMode`, `testMode`,
  `useSer`, `frameNumber`, `accumulatedFrameNumber`, `useWaitableSwapChain`,
  `frameLatencyWaitable`, DLSS-related statics
- `extern` declarations for helper functions used across files:
  `beginFrame()`, `submitCmd()`, `flush()`, `makeSlResource()`
- The `CHECK_SL_RESULT` macro (or move to its own tiny header)
- The param enum classes and `MAKE_PARAM` / `*_PARAM_IDX` macros (consumed by
  both pipeline init and render loop)

The actual variable definitions stay in whichever `.cpp` file is the "primary
owner." Use a single `renderer_state.cpp` that defines all the shared statics
if preferred, or just leave definitions scattered with `extern` in the header.

**Recommendation:** define all shared state in `renderer_state.cpp` to have one
canonical place. Makes it obvious where state lives.

### File breakdown

**`renderer.cpp`** (~300 lines) — orchestrator
- `init()`, `render()`, `loadScene()`, `resize()`, `flush()`, `destroy()`,
  `getFrameIndex()`, `queueResize()`
- `beginFrame()`, `submitCmd()`
- `needsResize`, `stopAccumulating`, `didPathTracingSettingsChange` flags
- Keeps `render()` which is the core logic and references everything

**`renderer_state.cpp`** (~100 lines) — shared state definitions
- All the `static` → now `extern` variable definitions
- `FrameContext` array, raw buffer ComPtrs, RtTarget instances, PSOs, root
  signatures, etc.

**`renderer_init.cpp`** (~400 lines) — device/infrastructure
- `initStreamline()`, `initDevice()`, `initDescriptorHeaps()`, `initNvapi()`,
  `initSwapChain()`, `initCommand()`, `initConstantParams()`
- `initRtTargets()` (the push_back setup, not resize logic)

**`renderer_pipeline.cpp`** (~500 lines) — root signatures + PSOs
- `initRootSignature()`, `initPipeline()`
- Static sampler setup, `makeParam()` helper
- The `MAKE_PARAM` macro can stay here or in `renderer_internal.h`

**`renderer_nrc.cpp`** (~100 lines) — NRC lifecycle
- `configureNrc()`, `initNrc()`, `destroyNrc()`

**`renderer_gui.cpp`** (~250 lines) — ImGui
- `initImgui()`, `imguiBeginFrame()`, `imguiEndFrame()`
- Combo option arrays, `debugViewComboMap`
- `updateFps()`, `frameTimeBuffer`
- Exposes `didPathTracingSettingsChange` and `needsResize` (write access from
  GUI callbacks — these two are shared with renderer.cpp)

**`renderer_screenshot.cpp`** (~130 lines) — screenshots
- `ScreenshotRequest` struct, `screenshotRequest` static
- `queueScreenshot()`, `captureQueuedScreenshot()`, `finalizeQueuedScreenshot()`

### Execution order

Do one file at a time. Suggested order:
1. `renderer_internal.h` + `renderer_state.cpp` — create the shared header,
   move variable definitions
2. `renderer_screenshot.cpp` — simplest, fewest dependencies
3. `renderer_nrc.cpp` — small, self-contained
4. `renderer_gui.cpp` — moderate size, clear boundary
5. `renderer_init.cpp` — bigger, but each `init*` function is self-contained
6. `renderer_pipeline.cpp` — biggest chunk, but mechanical
7. Clean up `renderer.cpp` — should be ~300 lines at this point

Build and test after each step.

### Tricky parts

- **`didPathTracingSettingsChange` and `needsResize`**: these are set by
  `imguiEndFrame()` (in gui.cpp) and read by `render()` (in renderer.cpp).
  Define them in `renderer_state.cpp`, extern in `renderer_internal.h`.

- **Static initialization order**: currently RtTargets are initialized as static
  globals with constructor args (`L"pathTracingTarget"`, format, channels). This
  is fine — their constructors don't depend on other statics. Just make sure
  `renderer_state.cpp` is the single definition site.

- **`autoTransitionRtTargets` / `allRtTargets`**: populated at runtime in
  `initRtTargets()`. Keep that function in `renderer_init.cpp`, vectors defined
  in `renderer_state.cpp`.

---

## Phase B: Pass Descriptors + Auto-Binding

After Phase A, `renderer.cpp` is ~300 lines but still has duplicated binding
code in `render()`. Phase B introduces two lightweight abstractions to fix that.

### B1: Root Signature Builder

Replace the 6 copy-paste root signature blocks with a builder.

```cpp
// renderer_pipeline.h

struct RootParamEntry {
    D3D12_ROOT_PARAMETER_TYPE type;
    uint32_t shaderRegister;
    uint32_t registerSpace;
};

ComPtr<ID3D12RootSignature> buildRootSignature(
    std::span<const RootParamEntry> params,
    std::span<const D3D12_STATIC_SAMPLER_DESC> staticSamplers = {},
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
```

Each root signature becomes a declaration:

```cpp
// Before: 30 lines of boilerplate per root signature
// After:
const RootParamEntry gbufferParamEntries[] = {
    { D3D12_ROOT_PARAMETER_TYPE_CBV, COMMON_REGISTER_GLOBAL_PARAMS, COMMON_REGISTER_SPACE },
    { D3D12_ROOT_PARAMETER_TYPE_SRV, RT_REGISTER_RAYTRACING_ACS,    RT_REGISTER_SPACE },
    { D3D12_ROOT_PARAMETER_TYPE_SRV, RT_REGISTER_VERTS,             RT_REGISTER_SPACE },
    // ...
    { D3D12_ROOT_PARAMETER_TYPE_UAV, GBUFFER_REGISTER_GBUFFER_OUT,  GBUFFER_REGISTER_SPACE },
};
gbufferRootSig = buildRootSignature(gbufferParamEntries, rtStaticSamplers);
```

The `MAKE_PARAM` macro and all the `*Param` enum classes disappear. Root param
index is now just the array index (0, 1, 2...), which can be `enum class` or
`constexpr uint32_t` per pass — but the point is the root sig and the enum are
derived from the same array, so they can't get out of sync.

**Saves ~200 lines** in `initRootSignature()` (currently ~260 lines, drops to
~60).

### B2: Scene Bindings Helper

The 8 "scene SRVs" (TLAS, verts, idxs, instanceDatas, materials, perTriDatas,
areaLights, areaLightSamplingStructure) are bound identically in gbuffer, NRC
update, and path tracing dispatches. Extract a helper:

```cpp
// scene_bindings.h

struct SceneBindings {
    D3D12_GPU_VIRTUAL_ADDRESS tlas;
    D3D12_GPU_VIRTUAL_ADDRESS verts;
    D3D12_GPU_VIRTUAL_ADDRESS idxs;
    D3D12_GPU_VIRTUAL_ADDRESS instanceDatas;
    D3D12_GPU_VIRTUAL_ADDRESS materials;
    D3D12_GPU_VIRTUAL_ADDRESS perTriDatas;
    D3D12_GPU_VIRTUAL_ADDRESS areaLights;
    D3D12_GPU_VIRTUAL_ADDRESS areaLightSamplingStructure;
};

SceneBindings getSceneBindings(const Scene& scene);

// Binds scene SRVs to consecutive root param indices starting at `baseIdx`.
void bindSceneResources(ID3D12GraphicsCommandList4* cmdList,
                        const SceneBindings& bindings,
                        uint32_t baseIdx);
```

The `RootParamEntry` arrays for gbuffer and PT would share a common "scene
resources" prefix. `bindSceneResources` does:
```cpp
void bindSceneResources(ID3D12GraphicsCommandList4* cmd,
                        const SceneBindings& b,
                        uint32_t base)
{
    cmd->SetComputeRootShaderResourceView(base + 0, b.tlas);
    cmd->SetComputeRootShaderResourceView(base + 1, b.verts);
    cmd->SetComputeRootShaderResourceView(base + 2, b.idxs);
    cmd->SetComputeRootShaderResourceView(base + 3, b.instanceDatas);
    cmd->SetComputeRootShaderResourceView(base + 4, b.materials);
    cmd->SetComputeRootShaderResourceView(base + 5, b.perTriDatas);
    cmd->SetComputeRootShaderResourceView(base + 6, b.areaLights);
    cmd->SetComputeRootShaderResourceView(base + 7, b.areaLightSamplingStructure);
}
```

This replaces 8 lines of `SetComputeRootShaderResourceView` at each call site
with 1 line:
```cpp
bindSceneResources(cmdList.Get(), sceneBindings, GBUFFER_SCENE_BASE_IDX);
```

**Saves ~50 lines** in `render()` (3 call sites x ~8 lines each, replaced by 3
one-liners).

### B3: Transition Batch (optional)

Lightweight barrier collector. Not a graph — just groups barriers into single
`ResourceBarrier` calls and validates no duplicate transitions:

```cpp
struct TransitionBatch {
    void add(ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after);
    void addUavBarrier(ID3D12Resource* resource = nullptr);
    void submit(ID3D12GraphicsCommandList4* cmdList);
};
```

Currently the code calls `stateTransitionResourceBarrier` one at a time (each
emits a separate `ResourceBarrier` call). Batching them is both cleaner and
marginally more efficient (D3D12 can overlap transitions submitted in one call).

This is the smallest win of the three and can be deferred if desired.

### Execution order for Phase B

1. **B1 first** — refactor `initRootSignature()` to use `buildRootSignature()`.
   This changes no runtime behavior; root signatures come out identical. Build
   and test.
2. **B2 second** — add `SceneBindings` helper, update the 3 call sites in
   `render()`. Build and test.
3. **B3 optional** — add `TransitionBatch`, update transition sites. Build and
   test.

### Things to watch out for

- **NRC buffer binding**: NRC update and NRC query bind `nrcBuffers` which are
  SDK-managed. These are NOT part of the scene bindings and should stay as
  explicit per-pass bindings. The scene bindings helper only covers the 8
  scene-data SRVs.

- **SER descriptor table**: path tracing root sig appends an extra descriptor
  table param when `useSer` is true. `buildRootSignature` needs to accept
  optional additional `D3D12_ROOT_PARAMETER1` entries (not just
  `RootParamEntry`) for this case. Simplest: let the builder accept a
  `std::span<const D3D12_ROOT_PARAMETER1> extraParams` that get appended after
  the `RootParamEntry`-derived params.

- **Root param indices must stay stable**: the scene bindings helper assumes
  scene SRVs are at consecutive indices starting at a known base. The
  `RootParamEntry` arrays must maintain this layout. This is already the case
  today (GLOBAL_PARAMS at 0, then scene SRVs at 1-8), so just keep that
  convention.

- **Postprocess/debug view root sigs**: these only have GLOBAL_PARAMS + static
  sampler. They're already simple. `buildRootSignature` handles them trivially
  but don't over-abstract — they're fine as 2-line calls.

---

## What NOT to do (and why)

**Full render graph (Option C):** NRC SDK's `BeginFrame`/`QueryAndTrain`/
`EndFrame` pattern and DLSS's `slEvaluateFeature` are opaque — they take the
command list and do their own barriers internally. A render graph that can't see
inside these calls would need escape hatches that undermine the automatic
barrier benefit. Revisit if these SDKs are dropped or if pass count grows
significantly.

**Extracting passes into classes with virtual dispatch:** the pass sequence is
fixed and conditional (NRC on/off, DLSS on/off). A flat `if`-based imperative
loop matches the actual control flow. Virtual dispatch adds indirection without
benefit here.
