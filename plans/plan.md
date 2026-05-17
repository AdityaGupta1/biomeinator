_Last edited: 2026-05-17_

> **Status:** Stages 1–4 complete. Stage 4 post-refactor (2026-05-17): RTSL logic
> lives entirely in `src/shaders/common/light_tree_sampling.hlsli`; `path_tracing.rgs.hlsl`
> only dispatches between sampling modes. Stages 5-7 pending. See `plans/cleanup.md`
> for remaining follow-ups.

> Design choice: **one light sample per pixel** (not paper-default of n samples per pixel). Plan picks a single subtree uniformly from the n-entry cut → `pdfSelect *= 1/n`. Cheaper sampling, relies on DLSS + temporal accumulation to denoise. Deviates from RTSL paper (which has each pixel sample all n subtrees).

> Scope: **HIS weights are pure `I · G / d²`** — the paper's BRDF reflectance bound `F` is omitted, matching the reference impl (`SLCHelperFunctions.hlsli::firstChildWeight`). Rationale: `luminance(F)` is a scalar shared by both children and cancels in the selection ratio, so F provides zero importance signal but introduces a degenerate uniform fallback for glossy-only materials. Dead-branch pruning is now purely geometric (`flux == 0 || geomTermBound == 0`) and valid for any BSDF — geometric back-face means no light path possible regardless of surface response. See `plans/cleanup.md` §1 for full rationale.

> Codebase note: emissive triangles already exist as `AreaLight` (`src/rendering/common/common_structs.h:163`), populated per-instance into `StructuredBuffer<AreaLight> areaLights` plus a flat selection index `areaLightSamplingStructure[]` (`src/scene/scene.cpp:480-565`). Triangles are flagged emissive by `perTriData.localAreaLightIdx != LIGHT_IDX_INVALID`, not by a `TRIANGLE_FLAG_*` bit (only `TRIANGLE_FLAG_IS_WATER` exists). Reuse `AreaLight`; add a parallel buffer for the per-light extras (bbox + flux) keyed by the same global area light index.


# Real-Time Stochastic Lightcuts — Implementation Plan

Goal: replace existing RIS many-light sampler with Real-Time Stochastic Lightcuts (RTSL) from Lin & Yuksel 2020 + Yuksel 2019. Must remain **unbiased** so existing golden-image tests still pass within their thresholds.

## Correctness invariants

The estimator must satisfy `E[L_estimate] = L_true` for every shading point. Concretely:

1. Each light primitive `i` with non-zero illumination contribution must have a non-zero selection probability `p_i > 0`.
2. The contribution must be divided by the exact selection pdf (`pdf_select * pdf_solid_angle`).
3. Dead branches return a *null* light sample (no light, but still count as a sample). Do not "retry" or backtrack — that introduces bias (see §3.2.2 of the offline paper).
4. MIS weighting against the BSDF sample uses the same `pdfSelect * pdfSolidAngle` as the area-light pdf. When the BSDF ray hits an emissive triangle, `pdfSelect` is recovered via `evaluateLightSelectPdf` (see Stage 4) — a root-to-leaf descent that recomputes the same HIS probabilities the forward sampler would have used at this shading point.
5. Bogus padding leaves have zero intensity → zero selection weight → never picked.
6. HIS random-number rescale (`r ← r/p1` on the picked-child branch, `r ← (r - p1)/(1 - p1)` on the other) reuses one uniform across the descent. Required for unbiased descent — see Yuksel 2019 Alg. 1, lines 11/15.

These rules together preserve the unbiased property of the existing path tracer. Goldens render at 2048 spp (`maxAccumulatedFrames=2048` in `tests/tests.json`, per-test threshold 0.001–0.015), so noise differences average out and the mean image matches.

---

## Stages

Each stage is mergeable on its own, with passing tests at the end.

### Stage 1 — Emitter Collection ✅ DONE (2026-05-13)

Goal: build per-frame flat buffer of emissive triangle primitives.

**Implementation notes (deviations from original task list above):**
- Both `dev_lightAux` and `dev_lightToLeaf` are keyed by the **sparse** `areaLights[]` index (not dense `samplingIdx`), so Stage 4's BSDF-hit recovery (`instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx`) indexes them directly. Sized by `Scene::getAreaLightSparseCount()` (high-water sparse index), pow2-stepped from a 256 floor.
- Two compute dispatches per topology change: `light_buffer_clear.cs.hlsl` zeroes/INVALIDs the full capacity, then `emitter_collect.cs.hlsl` overwrites live sparse slots. UAV barrier between. Clear ensures unreachable slots stay at sentinel even when the sparse layout shifts without a count change.
- Trigger: `Scene::areaLightTopologyChanged` flag set in `makeTlas`, cleared at top of `Scene::update`. Catches visibility toggles + delete-then-add same frame (which keep `numAreaLights` constant).
- `LightTreeManager` owns the buffers + two PSOs + two root sigs. `init()` once at renderer init; `reset()` for per-scene cleanup, `destroy()` for full teardown.
- `serializeAndCreateRootSignature` factored out of `renderer_pipeline.cpp` (static) into `renderer_internal.h` (extern) for reuse.
- Dispatch site is inside the `hasTlas() && (!stopAccumulating || ACCUMULATE)` block — flagged with TODO for Stage 2 to move out of accumulate gate.

**Tasks:**
- Add `LightAux` struct (HLSL + C++ mirror), parallel to existing `AreaLight` buffer, keyed by the same global area light index:
  - `float3 bboxMin`, `float3 bboxMax`
  - `float flux` (radiant power, area × emissive strength)
- Add `dev_lightAux` ManagedBuffer (resizable, sized to `numAreaLights`).
- Reuse existing `numAreaLights` count (`sceneParams.numAreaLights`).
- Emitter collection compute shader: one thread per `AreaLight`. Read `pos0/1/2_WS + instanceData.transformOffset` to compute world-space bbox and triangle area. Look up `Material[light.materialIdx]`; compute flux as:
  - `colorTerm = (emissiveColorTextureId == TEXTURE_ID_INVALID) ? luminance(emissiveColor) : 1.0` — textured emitters use float3(1) proxy for simplicity, untextured uses actual color luminance.
  - `flux = emissiveStrength * colorTerm * triangleArea`.
  - Used only as importance weight, so approximation is fine — looser flux means higher noise but no bias.
- Add `dev_lightToLeaf` raw buffer: `uint[numAreaLights]` mapping `areaLightIdx → leafIdx`. Initialize to `LEAF_IDX_INVALID` (covers slots from bogus leaves, never written by scatter). Written in Stage 2 after sort. Needed for `evaluateLightSelectPdf` (MIS BSDF→light pdf lookup).
- For glTF mode: rebuild only when scene topology changes (i.e. when `areaLightSamplingStructure` gets rewritten — `scene.cpp:553-565`).
- For voxel mode: rebuild when chunks dirty (cheap — emissive blocks are few).

**Validation:** covered transitively by Stage 2's root-flux invariant (root flux == sum of `LightAux.flux`). No render changes yet.

### Stage 2 — Light Tree Build (single-level)

Goal: GPU-side perfect binary tree, rebuilt on the same `Scene::areaLightTopologyChanged` flag that gates Stage 1. Tree staleness ≡ Stage 1 staleness — both consume the same trigger because Stage 2 is a pure function of `dev_lightAux`. (When dynamic transforms or runtime emissive-strength changes land later, the fix is to expand Stage 1's trigger; Stage 2 inherits it for free.)

**Storage convention:** 0-indexed perfect binary tree. `tree[0]` = root. Children of node `i` are `tree[2i + 1]` and `tree[2i + 2]`. Parent of node `i > 0` is `tree[(i - 1) / 2]`. For pow2 leaf count `M = nextPow2(numAreaLights)`, total nodes = `2M - 1`, leaves occupy `tree[M - 1 .. 2M - 2]` (so `treeLeafBase = M - 1`).

**Tasks:**

- Add `LightTreeNode` (32 B):
  - `float3 bboxMin`, `float3 bboxMax`
  - `float flux`
  - `uint areaLightIdx` (leaves only; internal nodes + bogus leaves set to `LIGHT_IDX_INVALID`, defined as `~0u` and shared with Stage 1's existing sentinel)
- Add `dev_lightTree` raw buffer, allocated via `BufferHelper::createBasicBuffer` and resized via `Util::nextPow2AtLeast(LIGHT_TREE_NODE_FLOOR, 2 * numAreaLights - 1)` with old buffers pushed to `toFreeList` (matches Stage 1's pattern in `light_tree_manager.cpp:120-152`, **not** `ManagedBuffer`). Set `LIGHT_TREE_NODE_FLOOR = 511` (= `2 * LIGHT_AUX_CAPACITY_FLOOR - 1`).
- Add `dev_sceneBbox` permanent UAV (`6 × float`). Allocated once in `LightTreeManager::init`; never resized.
- Add `dev_mortonKeys` + `dev_mortonValues` raw buffers (uint32 each), sized to `nextPow2AtLeast(MORTON_FLOOR, numAreaLights)`. Resize idiom matches `dev_lightTree`.
- **Guard whole Stage 2 dispatch on `numAreaLights > 0`.** `GpuRadixSort::dispatch` asserts on `numKeys == 0`; the empty-scene case leaves the tree untouched and Stage 4's sampler bails on the root's zero flux.

**Compute passes (in order):**

0. **scene bbox reset** — single dispatch `(1,1,1)`, writes 6 orderable-uint sentinels (`+inf` mins, `-inf` maxes) into `dev_sceneBbox` so the bbox-reduce atomics start from the widest possible state. Added during implementation; the original plan implicitly assumed bbox-reduce would seed itself, but cross-workgroup atomics can't safely seed in-shader.
1. **bbox reduce** — parallel reduce over `dev_lightAux[0..capacity]`, writing min/max into `dev_sceneBbox`. Reads `LightAux` directly — Stage 1 already folds `instanceData.transformOffset` into the WS bbox (`emitter_collect.cs.hlsl:42-47`). Sparse holes carry Stage 1's inverted-infinity sentinels, so the union absorbs them with no live-slot branch. Implementation: workgroup-cooperative shared-memory reduce + atomic min/max into `dev_sceneBbox` across workgroups.
2. **morton emit** — dispatch `numAreaLights` threads (dense, **not** sparse capacity). Each thread `samplingIdx` reads `sparseIdx = areaLightSamplingStructure[samplingIdx]`, then `LightAux[sparseIdx]`, then writes `(mortonKey32, sparseIdx32)` into `dev_mortonKeys[samplingIdx]` / `dev_mortonValues[samplingIdx]`. 30-bit Morton (10 bits per axis, per RTSL §4) packed into a 32-bit key (top 2 bits zero). Pow2 padding lives only in the tree, not in the sort — sort sees no holes, no sentinels. Writing a new `morton30()` helper in HLSL (no existing one in the repo).
3. **sort** — `gpuRadixSort.dispatch(cmdList, toFreeList, dev_mortonKeys, dev_mortonValues, numAreaLights)`. In-place ping-pong (Stage 3); result lands back in caller buffers.
4. **leaf populate + reverse scatter (fused)** — dispatch `M` threads, one per leaf slot. Slot `s < numAreaLights`: read `sparseIdx = dev_mortonValues[s]`, fetch `LightAux[sparseIdx]`, write `lightTree[treeLeafBase + s]` from it (set `areaLightIdx = sparseIdx`), and scatter `lightToLeaf[sparseIdx] = treeLeafBase + s`. Slot `s >= numAreaLights`: write a sentinel node (inverted-infinity bbox, flux=0, `LIGHT_IDX_INVALID`); skip the scatter — Stage 1's pre-clear keeps holes at `LEAF_IDX_INVALID`. Replaces the old separate "leaf populate" + "reverse scatter" passes (saves a dispatch and a UAV barrier).
5. **internal levels** — bottom-up gather, **d = 2** per dispatch (each thread reads 4 grandchildren, writes parent + 2 intermediate children). `ceil(log2(M)/2)` dispatches with a UAV barrier between each; when `log2(M)` is odd the last dispatch drops to `d = 1` (via a root-constant flag). Simple per-thread arithmetic, no shared-memory cooperation needed. Bogus-leaf sentinels propagate cleanly through the union (zero flux, no bbox contribution).

**No separate `dev_lightTree` clear pass needed:** the fused leaf-populate writes every leaf slot (real or sentinel) and the internal-levels passes write every internal node. Saves a dispatch on resize-frames too.

**Validation (CPU readback):**
- Root bbox encloses all live `LightAux` bboxes.
- Root flux == sum of live `LightAux.flux` (within FP tolerance).
- Bogus leaves (slots `[numAreaLights, M)`): bbox = inverted-infinity sentinel, flux = 0, `areaLightIdx == LIGHT_IDX_INVALID`.
- **Per-level invariant:** every internal node's flux == sum of its two children's flux, and every internal node's bbox == union of its two children's bboxes. Walk the full tree, not just the root — catches multi-level dispatch bugs (e.g. a level silently skipped) that a root-only check would miss.

**Total dispatch count:** `4 + ceil(log2(M)/2)` (scene bbox reset, bbox reduce, morton emit, fused leaf populate, then `ceil(log2(M)/2)` internal-levels dispatches), plus the 4 internal sort passes Stage 3 owns. For `M = 4096` that's 10 dispatches plus 4 sort passes.

**Floor constants:** implementation collapses the plan's separate `LIGHT_TREE_NODE_FLOOR` / `MORTON_FLOOR` into a single `LIGHT_TREE_LEAF_FLOOR = 256` (matching the Stage 1 `LIGHT_AUX_CAPACITY_FLOOR`). Tree node count is derived as `2M - 1`, Morton buffers size to `M`. Single floor is simpler and the two derived sizes can never drift.

### Stage 3 — Sort Library ✅ DONE (2026-05-15)

Goal: GPU radix sort over 32-bit Morton key + 32-bit `areaLightIdx` payload (pairs mode), ascending.

**Decision:** [GPUSorting (b0nes164)](https://github.com/b0nes164/GPUSorting) — DeviceRadixSort (LSD, 8-bit digits, 4 passes). Reduce-then-scan variant for portability (OneSweep relies on forward thread-progress guarantees we don't want to assume across hardware). See `knowledge/rendering/gpu_radix_sort.md` for tuning rationale and the in-place ping-pong invariant.

**Implementation notes (no deviations from spec):**
- Submodule lives at `external/GPUSorting`. CMake compiles `DeviceRadixSort.hlsl` four times (one per entry point) with `-DSORT_PAIRS -DKEY_UINT -DPAYLOAD_UINT -DSHOULD_ASCEND -DENABLE_16_BIT`.
- `Renderer::GpuRadixSort` (`src/rendering/gpu_sort/gpu_radix_sort.{h,cpp}`) owns the 4 PSOs/root sigs and scratch buffers; exposes `dispatch(cmdList, toFreeList, keysBuf, valuesBuf, numKeys)` with in-place semantics (the 4-pass ping-pong lands the result back in caller storage).
- Stage 2 will call `dispatch` after writing `(mortonCode, areaLightIdx)` into a pair of UAV buffers.

**Validation:** verified by a temporary smoke test run at renderer init for n ∈ {1, 7680, 7681, 1<<20} — asserted ascending keys + payload-tracks-key. Test passed on RTX 4070 SUPER and was removed (Stage 2 will exercise the path going forward).

### Stage 4 — Root Sampler (No Cuts) ✅ DONE (2026-05-17)

Goal: replace RIS branch with stochastic light tree sampling from root. No cut selection yet.

**Implementation notes (post-refactor state):**
- `samplingMode == RTSL` (added to `SamplingMode` enum in `src/rendering/common/common_enums.h`). Existing modes retained for A/B.
- All RTSL logic lives in `src/shaders/common/light_tree_sampling.hlsli`:
  - `rtslChildProbs(c1, c2, hitPos, hitNormal, out p1, out p2)` — paper's `p_j = 0.5 * (p_min_j + p_max_j)` with `w_min/max = I · G / d_{min/max}²`. F factor dropped (see front-matter scope note).
  - `geomTermBound(p, N, bboxMin, bboxMax)` — tangent-frame projection bound from Lin & Yuksel 2020 (ref impl `LightTreeUtilities.hlsli`): `nrm_max / sqrt(nrm_max² + y_amin² + z_amin²)`. True upper bound on cos angle over bbox interior.
  - `selectLightFromSubtree(subtreeRoot, hitPos, hitNormal, rng, out areaLightIdx, out pdfSelect)` — Alg. 1 of offline paper. Single uniform rescaled across descent (Yuksel 2019 Alg. 1 lines 11/15). Dead branch → null light, no retry.
  - `evaluateLightSelectPdf(areaLightIdx, hitPos, hitNormal)` — MIS BSDF→light recovery. Leaf-index bits as the path, MSB-first. Returns 0 on dead ancestor or sentinel leaf. Stage 5 will extend to support non-root subtree.
  - `sampleDirectLightingRtsl(...)` — NEE entry point mirroring `sampleDirectLightingUniform`. Calls `selectLightFromSubtree` + `sampleAreaLightPoint` + `traceToLight`.
  - `lightPdfRtsl(hitInfo, surfPos_WS, surfNor_WS, wi_WS)` — BSDF-hit MIS pdf, mirroring `lightPdfUniform`'s signature.
  - Edge case: x inside both child bboxes → drop distance term from `w_min`; `w_max` retains `1 / d_max²`. Per RTSL §3.2 last paragraph.
- `light_sampling.hlsli` factored: `sampleAreaLightPoint` (shared triangle-area sampler used by uniform + RTSL), `getAreaLightIdxFromHit` (shared hit-decode used by both pdf functions).
- `path_tracing.rgs.hlsl` `useRtsl` branches are one-liners: `sampleDirectLightingRtsl(...)` for NEE, `lightPdfRtsl(...)` for BSDF-hit MIS. Same shape as `useRis` dispatch.
- Specular surfaces never enter NEE (`!isDeltaSurface` gate, see `path_tracing.rgs.hlsl`).
- Null light from `selectLightFromSubtree`: skip shadow ray, add zero contribution. Still counts as one sample (no retry — would introduce bias per offline paper §3.2.2).

**Validation:**
- Add tests `cornell_box_sl`, `cave_lights_sl`, `many_faces_emitter_sl` with `--samplingMode=3`, same goldens + threshold as their MIS/RIS variants. Goldens unchanged — unbiased estimator converges to same mean.
- Bias sanity check: compare 16k-sample SL render to 16k-sample naive render, max per-channel error < 0.005.

### Stage 5 — Cut Selection + Sharing

Goal: 8×8 tile cut sharing for perf. Pure speed — must not affect bias.

**Tasks:**
- Add `dev_tileCuts` raw buffer: `numTilesX * numTilesY * n` uint node indices (n = samples per tile).
- New compute shader `lightcuts_cut_selection.cs.hlsl`:
  - One thread per tile.
  - Pick representative pixel by scanning the 64 tile pixels in an order seeded by `(tile, frameIdx)` — **must vary per frame** for temporal stability (paper §3.3 randomizes per frame; this is equivalent). Read each candidate's entry from `dev_gbuffer` (flat `StructuredBuffer<GbufferData>`, indexed by `pixelIdx.y * renderSize.x + pixelIdx.x`; see `path_tracing.rgs.hlsl:46, 541`). Take the first pixel that has `payload.flags & PAYLOAD_FLAG_DID_HIT`, a valid `materialIdx`, and a non-delta material. All 64 pixels fail → write `cut[0] = INVALID` and skip light sampling for the tile.
  - Build cut: start with [root], repeatedly replace highest-error node with its children until `n` nodes. Error metric = max-illumination bound per RTSL §3.2 (use same `w_max_j` formula as importance sampling so consistent).
- In path tracing: **one light sample per pixel.** Read cut for current tile, pick one subtree uniformly (1/n), descend via HIS using **this pixel's own** `(surfPos, surfNor)` — the cut is shared, but the descent weights are pixel-local. Deviates from RTSL paper (which samples all n cut entries per pixel). Single-sample chosen for perf; DLSS + temporal accumulation handle the extra noise.
- Extend `evaluateLightSelectPdf` for cut sharing: walk leaf → root via parent indices (`parent = (i - 1) / 2`), scan cut entries (n ≤ ~8) for first ancestor in the cut. Found → `pdfSelect = (1/n) * P(descend ancestor → leaf)`. Not found (cut entry mismatch — shouldn't happen with full cut, possible later if interleaved sampling lands) → `pdfSelect = 0`. Cost is O(log N · n) per BSDF-hit pdf eval — acceptable; flag if profiling later objects.
- **Invalid-cut tile (`cut[0] == INVALID`): skip NEE entirely for the pixel.** Both forward NEE and BSDF-hit `evaluateLightSelectPdf` must agree on the selection pdf or MIS double-counts. Skipping NEE collapses MIS to BSDF-only on both sides — consistent and unbiased. Forward sampler and pdf eval check the same `cut[0] == INVALID` flag and bail.
- Tile size = 8×8. n configurable, default 8.

**Notes for bias:**
- Subtree selection probability inside the cut = 1/n (uniform), so `pdfSelect *= 1/n`.
- Cut sharing across pixels does NOT create correlation in light selection (different RNG per pixel — `initRng(..., linearPixelIdx, ...)` in `path_tracing.rgs.hlsl:548-550`).
- The cut itself depends on rep pixel — but the union of subtrees covers all leaves (perfect tree partition), so every light still has p > 0 at the rep pixel. At a *non-rep* pixel the HIS uses that pixel's own bounds, so dead branches can shift; this is fine — null lights still count, and BSDF-MIS recovery handles the dead-branch pdf via `pdfSelect = 0`.
- Tile with no valid rep pixel (all delta or all miss): `cut[0] = INVALID` → both forward NEE and BSDF-hit pdf eval skip light sampling for the pixel (BSDF-only path). Consistent on both sides → no MIS double-count.

**Validation:**
- All SL tests still pass with `samplingMode=3` (now using cut sharing).
- Visual A/B: noise should drop or hold steady, never significantly increase, at fixed render time.

### Stage 6 — Two-Level Tree (voxel-mode optimization)

Goal: per-chunk bottom-level trees + top-level over chunk roots. Skip if not needed after profiling.

**Tasks:**
- Per-chunk `LightTreeNode[]` allocation (sized to chunk's emissive count, pow2 padded).
- Top-level tree over non-empty chunks' root nodes.
- Rebuild bottom-level only for dirty chunks; top-level every frame (cheap).
- HIS traversal: descend top-level → reach a chunk-root leaf → recurse into that chunk's bottom-level tree.
- Storage: track `chunkLightTreeOffset[]` (where each chunk's bottom-level tree starts).
- Reverse map for MIS BSDF-hit pdf eval: `lightToChunk[areaLightIdx] → chunkIdx`. `evaluateLightSelectPdf` looks up chunk, walks top-level root → chunk-root leaf, then chunk-bottom root → leaf within that chunk. `dev_lightToLeaf` becomes `dev_lightToLocalLeaf` (leaf index within its chunk's bottom-level tree).

**Validation:** voxel tests (`cave_lights`, `underwater`) still pass. Profile: bottom-level rebuild < 0.1ms per dirty chunk.

### Stage 7 — Optional Interleaved Sampling (deferred)

Sub-block (2×2 within tile) takes disjoint subset of cut subtrees. Effectively doubles cut depth without per-pixel cost. Only do if Stage 6 profiling shows headroom is worth the SVGF tuning cost. Probably skip for v1.

---

## File touch list (Stages 1–5)

```
src/
  rendering/
    light_tree_manager.h/.cpp           NEW — owns build pipeline, buffers
    renderer/
      renderer.cpp                      EDIT — invoke build before path trace
    common/
      common_enums.h                    EDIT — add STOCHASTIC_LIGHTCUTS to SamplingMode
  shaders/
    common/
      light_aux.hlsli                    NEW — LightAux struct + buffer decls (bbox + flux, parallel to AreaLight)
      light_tree.hlsli                   NEW — node struct + traversal helpers
      light_tree_sampling.hlsli          NEW — HIS sampler
    light_tree/
      emitter_collect.cs.hlsl            NEW — fill LightAux from AreaLight + Material
      light_bbox_reduce.cs.hlsl          NEW
      morton_code.cs.hlsl                NEW
      light_tree_leaves.cs.hlsl          NEW
      light_tree_internals.cs.hlsl       NEW
      cut_selection.cs.hlsl              NEW (Stage 5)
    path_tracing/
      path_tracing.rgs.hlsl              EDIT — dispatch to sampleDirectLightingRtsl / lightPdfRtsl
external/
  GPUSorting/                            NEW (submodule)
tests/
  tests.json                             EDIT — add *_sl variants
knowledge/
  scene/
    area_lights.md                       NEW — area light system + LightAux extension
  rendering/
    light_tree.md                        NEW (Stage 2)
    render_passes.md                     EDIT — add light tree build pass
```

---

## Risk + mitigation

| Risk | Mitigation |
|---|---|
| Geometric bound too loose for voxel materials (under-importance distant lights) | Compare HIS noise vs RIS on `many_faces_emitter`. Tighten bound (per-face cone) if needed. |
| Dead branch handling bug → bias | Unit-test the HIS sampler on a CPU port: 1M samples, verify mean matches uniform sampling. |
| Cut selection picks bad rep pixel (specular, miss) | Skip NEE for pixel when `cut[0] == INVALID`; both sides of MIS agree. |
| MIS pdf mismatch between forward sampler and `evaluateLightSelectPdf` (silent bias) | Unit-test on CPU port: for fixed `x, normal`, run sampler N times, build empirical leaf histogram, compare to `evaluateLightSelectPdf` predictions. χ² should match. |

---

## Sources

- [Real-Time Stochastic Lightcuts (Lin & Yuksel)](https://www.cemyuksel.com/research/stochasticlightcuts/realtime_stochastic_lightcuts.pdf)
- [Stochastic Lightcuts (Yuksel TVCG 2020)](https://www.cemyuksel.com/research/stochasticlightcuts/stochasticlightcuts_tvcg.pdf)
- [GPUSorting (b0nes164)](https://github.com/b0nes164/GPUSorting)
- [AMD FidelityFX Parallel Sort](https://gpuopen.com/fidelityfx-parallel-sort/)
- [Reference impl from paper authors](https://github.com/DQLin/RealTimeStochasticLightcuts)
