_Last edited: 2026-05-15_

> **Status:** Stage 1 complete (2026-05-13). Sparse-keyed `LightAux` + `lightToLeaf` buffers, two-pass clear+collect dispatch, topology-change trigger from `Scene::didAreaLightTopologyChange()`. Stages 2-7 pending.

> Design choice: **one light sample per pixel** (not paper-default of n samples per pixel). Plan picks a single subtree uniformly from the n-entry cut → `pdfSelect *= 1/n`. Cheaper sampling, relies on DLSS + temporal accumulation to denoise. Deviates from RTSL paper (which has each pixel sample all n subtrees).

> Scope: **v1 bounds the diffuse lobe only.** Reflectance bound `F = albedo / π * max_dot(normal, x, nodeBbox)` is a true upper bound for the diffuse lobe but not for glossy lobes. Materials with both diffuse and glossy components still get sampled (selection probability stays > 0 wherever the diffuse lobe contributes), but dead-branch pruning (`w_max == 0`) is only valid where the diffuse-only bound is the full BSDF bound. Until glossy bounds land, gate dead-branch pruning to materials with `hasDiffuse() && !hasGlossy()`; mixed materials descend the full tree without pruning. Glossy lobes add variance, not bias.

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

Goal: GPU-side perfect binary tree built every frame.

**Tasks:**
- Add `LightTreeNode` (32 B):
  - `float3 bboxMin`, `float3 bboxMax`
  - `float flux`
  - `uint areaLightIdx` (leaf only; internal nodes set to `LIGHT_IDX_INVALID`)
- Add `dev_lightTree` ManagedBuffer (sized to next pow2 of light count × 2).
- Compute passes (in order):
  1. **bbox reduction** — parallel reduce over `dev_lightAux` → scene bbox UAV.
  2. **morton code** — write `(mortonCode, areaLightIdx)` pairs into separate key/value buffers. 30-bit Morton (10 bits per axis, per RTSL §4) stored as a 32-bit key (top 2 bits zero); `areaLightIdx` is the 32-bit payload. The sort library (Stage 3) uses pairs-mode sort, so no 64-bit packing is needed.
  3. **sort** — see "Sort library" below.
  4. **leaf populate** — copy sorted lights into bottom level; bogus leaves get zero flux + degenerate bbox + `LIGHT_IDX_INVALID`.
  5. **internal levels** — bottom-up gather in level groups. Multiple levels per dispatch (level ℓ built directly from level ℓ + d). Use thread = node, gather 2^d children.
  6. **write reverse index** — fill `dev_lightToLeaf` by scattering: each non-bogus leaf thread reads its leaf node's `areaLightIdx`, writes its own `leafIdx` at that slot. Bogus leaves skip the write (buffer pre-initialized to `LEAF_IDX_INVALID`).
- Resize tree buffer when light count crosses pow2 boundary.

**Validation:** CPU readback, verify:
- Root bbox encloses all lights.
- Root flux == sum of leaf fluxes (within FP tolerance).
- Bogus leaves all zero.

### Stage 3 — Sort Library ✅ DONE (2026-05-15)

Goal: GPU radix sort over 32-bit Morton key + 32-bit `areaLightIdx` payload (pairs mode), ascending.

**Decision:** [GPUSorting (b0nes164)](https://github.com/b0nes164/GPUSorting) — DeviceRadixSort (LSD, 8-bit digits, 4 passes). Reduce-then-scan variant for portability (OneSweep relies on forward thread-progress guarantees we don't want to assume across hardware). See `knowledge/rendering/gpu_radix_sort.md` for tuning rationale and the in-place ping-pong invariant.

**Implementation notes (no deviations from spec):**
- Submodule lives at `external/GPUSorting`. CMake compiles `DeviceRadixSort.hlsl` four times (one per entry point) with `-DSORT_PAIRS -DKEY_UINT -DPAYLOAD_UINT -DSHOULD_ASCEND -DENABLE_16_BIT`.
- `Renderer::GpuRadixSort` (`src/rendering/gpu_sort/gpu_radix_sort.{h,cpp}`) owns the 4 PSOs/root sigs and scratch buffers; exposes `dispatch(cmdList, toFreeList, keysBuf, valuesBuf, numKeys)` with in-place semantics (the 4-pass ping-pong lands the result back in caller storage).
- Stage 2 will call `dispatch` after writing `(mortonCode, areaLightIdx)` into a pair of UAV buffers.

**Validation:** verified by a temporary smoke test run at renderer init for n ∈ {1, 7680, 7681, 1<<20} — asserted ascending keys + payload-tracks-key. Test passed on RTX 4070 SUPER and was removed (Stage 2 will exercise the path going forward).

### Stage 4 — Root Sampler (No Cuts)

Goal: replace RIS branch with stochastic light tree sampling from root. No cut selection yet.

**Tasks:**
- Add `STOCHASTIC_LIGHTCUTS` to `SamplingMode` enum in `src/rendering/common/common_enums.h` (alongside `NAIVE`, `MIS`, `RIS`). Existing modes stay so we can A/B during dev.
- Add `light_tree_sampling.hlsli`:
  - `selectLightFromSubtree(uint subtreeRoot, float3 hitPos, float3 hitNormal, float3 brdfBound, inout RNGState rng, out uint areaLightIdx, out float pdfSelect)` — implements Alg. 1 of offline paper. Uses the RTSL weight scheme: `p_j = 0.5 * (p_min_j + p_max_j)` with `w_min/max = F * I / d_{min/max}²`. Single uniform rescaled across descent (Yuksel 2019 Alg. 1 lines 11/15). Dead branch (w1+w2==0 at any depth) → return null light + current pdf.
  - `evaluateReflectanceBound(float3 hitPos, float3 hitNormal, float3 brdf, LightTreeNode node)` — Lambertian: `albedo / π * max_dot_to_bbox(normal, x, nodeBbox)`. Diffuse-only bound — see scope note in front-matter. Materials with glossy lobes still descend the tree (selection probability stays > 0 wherever diffuse contributes), but dead-branch pruning is suppressed (`hasGlossy() → never treat w_max == 0 as dead`). Specular surfaces never call into the sampler (existing path tracer already gates light sampling on `!isDeltaSurface`, see `path_tracing.rgs.hlsl:260`).
  - `evaluateLightSelectPdf(uint areaLightIdx, float3 hitPos, float3 hitNormal, float3 brdfBound)` — for MIS BSDF→light direction. Looks up `leafIdx = lightToLeaf[areaLightIdx]`; bogus leaf → return 0. Descend root → leaf using leaf-index bits as the path. At each internal node computes the same `w_min/w_max` weights, multiplies in the probability of the on-path child. Returns 0 if any ancestor is a dead branch (`w1 + w2 == 0`) at this shading point. Stage 5 extends this to handle cut sharing.
  - Edge case: x inside both child bboxes (both `d_min == 0`) → drop distance term from `w_min` only (`w_min_j = F_j * I_j`); `w_max` retains `1 / d_max²` since `d_max > 0` always. Per RTSL §3.2 last paragraph.
- In `path_tracing.rgs.hlsl`:
  - Step 7 (NEE), when `samplingMode == STOCHASTIC_LIGHTCUTS`: call `selectLightFromSubtree(root, ...)` per sample. Reuse existing `traceToLight(...)` (`src/shaders/light/light_sampling.hlsli`) for the shadow ray — it already validates `instanceId/triangleIdx` against the chosen `AreaLight`. Accumulate `L * f * cosθ * V / (pdfSelect * pdfSolidAngle)`. MIS weight uses `pdfSelect * pdfSolidAngle` as the area pdf.
  - Step 10 (BSDF-hit emission MIS): existing code reads `payload.hitInfo`, recovers `areaLightIdx = instanceDatas[hitInfo.instanceId].areaLightsBufferOffset + perTriDatas[…].localAreaLightIdx` (mirrors `lightPdfUniform` in `src/shaders/light/light_sampling.hlsli:137-156`). Then `pdfSelect = evaluateLightSelectPdf(areaLightIdx, surfPos_WS, surfNor_WS, prevBrdfBound)` and `areaPdf = pdfSelect * r² / (absCosTheta(-wi, lightNor) * lightArea)`. `pdfSelect == 0` (dead branch / cut miss) → MIS weight collapses to 1 (BSDF-only).
  - Stash `prevBrdfBound` at the last-real-bounce alongside the existing `surfPos_WS / surfNor_WS / bounceBsdfPdf` (`path_tracing.rgs.hlsl:104-106, 203-207`). Single `float3` (Lambertian albedo evaluated at last bounce); zero for delta surfaces (no light-sample branch was taken).
- Null light from `selectLightFromSubtree`: skip shadow ray, add zero contribution (but still count toward sample budget for unbiased MIS weighting).

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
- In path tracing: **one light sample per pixel.** Read cut for current tile, pick one subtree uniformly (1/n), descend via HIS using **this pixel's own** `(surfPos, surfNor, brdfBound)` — the cut is shared, but the descent weights are pixel-local. Deviates from RTSL paper (which samples all n cut entries per pixel). Single-sample chosen for perf; DLSS + temporal accumulation handle the extra noise.
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
      path_tracing.rgs.hlsl              EDIT — swap RIS branch, stash prevBrdfBound
external/
  GPUSorting/                            NEW (submodule)
tests/
  tests.json                             EDIT — add *_sl variants
knowledge/
  shaders/
    light_tree.md                        NEW
  scene/
    area_lights.md                       NEW — area light system + LightAux extension
  rendering/
    render_passes.md                     EDIT — add light tree build pass
```

---

## Risk + mitigation

| Risk | Mitigation |
|---|---|
| Reflectance bound too loose for voxel materials (under-importance distant lights) | Compare HIS noise vs RIS on `many_faces_emitter`. Tighten bound (per-face cone) if needed. |
| Dead branch handling bug → bias | Unit-test the HIS sampler on a CPU port: 1M samples, verify mean matches uniform sampling. |
| Cut selection picks bad rep pixel (specular, miss) | Skip NEE for pixel when `cut[0] == INVALID`; both sides of MIS agree. |
| MIS pdf mismatch between forward sampler and `evaluateLightSelectPdf` (silent bias) | Unit-test on CPU port: for fixed `x, normal, brdf`, run sampler N times, build empirical leaf histogram, compare to `evaluateLightSelectPdf` predictions. χ² should match. |

---

## Sources

- [Real-Time Stochastic Lightcuts (Lin & Yuksel)](https://www.cemyuksel.com/research/stochasticlightcuts/realtime_stochastic_lightcuts.pdf)
- [Stochastic Lightcuts (Yuksel TVCG 2020)](https://www.cemyuksel.com/research/stochasticlightcuts/stochasticlightcuts_tvcg.pdf)
- [GPUSorting (b0nes164)](https://github.com/b0nes164/GPUSorting)
- [AMD FidelityFX Parallel Sort](https://gpuopen.com/fidelityfx-parallel-sort/)
- [Reference impl from paper authors](https://github.com/DQLin/RealTimeStochasticLightcuts)
