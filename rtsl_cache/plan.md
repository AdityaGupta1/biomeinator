# RTSL Screen-Space Tile Cache Implementation Plan (v3)

_Last edited: 2026-05-23_

## Changelog

- **v3 (2026-05-23):** step-1 implementation deltas. `L` default raised to 5 (UI range 3–7); `uniformFrac` min raised to 0.05. Scene-cut suppression narrowed to **frame 0 + settings change only** — `globalInstanceOffset` rebase and light-tree topology change are deliberately NO LONGER suppressed (see "Scene-cut handling" for rationale). `depthBucketScale` default set to 0.3333.
- **v2 (2026-05-22):** audit-driven revision. Critical fixes folded inline. See "v1 → v2 deltas" appendix at the bottom for the explicit list. Major changes: pdf path now gated on `pathDepth == 0` (was ambiguous); NRC cache use is "on in both UPDATE+QUERY or off in both" (was "off in update, on in query" — biased); `MAX_RENDER_SIZE` dropped (didn't exist) — buffers reallocate on resize; pdf-hoist moved to step 4 (perf risk on critical path); explicit state-transition table in step 6; `linearDepthPrev` pulled out of `autoTransitionRtTargets`; light-tree-topology-change force-clears Prev; scene-cut detector beyond frame 0; shared `tcSlotAccepts` predicate as single source of truth.

## Background

Prior plan (`plans/rtsl_improvements`, archived) used a world-space hash grid layered on stochastic lightcuts. Pulled due to patent overlap on the hash-grid + lightcuts pairing. This plan replaces the hash-grid key with a screen-space tile + sub-bucket key. Everything downstream of the key (K-slot payload, ancestor walk, mixture pdf, MIS unbiasedness) is identical, so the math + Python emulator from the prior plan stay valid after adapting the lookup shape.

Prior art for screen-space tile light buckets: tiled deferred shading, clustered shading, ReSTIR spatial reservoirs. Patent risk on tile-based bucketing is effectively zero.

## Scope

**Primary hit only.** Cache reads and writes happen ONLY when `pathDepth == 0` AND `pathSplitIdx == 0` (the diffuse first-bounce lane). Both `lcSelectSubtreeRoot` (NEE sampler) and `lcEvaluateMixturePdf` (BSDF-hit emission MIS) must be gated identically — see "Gating discipline" below.

Indirect bounces, BSDF-hit-emission at depth > 0, and the NRC update variant all use the **uniform-root** RTSL path with `evaluateLightSelectPdf` (the existing pre-cache code). NRC is expected to amplify the improved first-bounce direct-lighting quality into the indirect domain via training; if direct quality improves, NRC training targets get cleaner and downstream indirect improves transitively.

## Goals

Reduce RTSL direct-light NEE miss rate on first bounce by starting the HIS descent `L` levels above a cached light instead of at the tree root. Mixture with root descent (`uniformFrac`) keeps full coverage and unbiased estimation. Default `L = 5` (UI range 3–7); revisit after A/B.

## Architecture overview

Per-pixel surface attributes (depth, normal, position) are known after the G-buffer pass. Tile index = `pixel.xy / 8`. Within a tile, four **depth sub-buckets** split capacity by log-depth band so a near surface doesn't get evicted by a far surface in the same tile. Each sub-bucket holds `K = 32` slots of `(lightIdx u32, normalOct u32)`. Normal tag enables per-slot soft-reject when the stored slot's normal disagrees with the current pixel's normal.

Per-frame loop:
1. G-buffer pass — produces linear depth, normals+roughness, motion vector (already written by `gbuffer.rgs.hlsl`).
2. Ping-pong heap indices (tile cache A↔B, linear depth Curr↔Prev).
3. State transitions: incoming Prev buffers UAV → `NON_PIXEL_SHADER_RESOURCE`; incoming Curr buffers (whatever was Prev last frame) `NON_PIXEL_SHADER_RESOURCE` → UAV.
4. Clear `tileCacheCurr` (compute dispatch).
5. Path tracing — reads `tileCachePrev` + `linearDepthPrev` + `motion`; writes `tileCacheCurr` and `linearDepthCurr`.

No eviction pass: fixed-size grid, no hash table, slots are overwritten by the next frame's inserts and naturally die when no pixel keeps voting for them. Reprojection (motion vector + disocclusion check) is the temporal-continuity mechanism.

## Gating discipline

The cache is read AND written under the IDENTICAL gate so the MIS pair stays unbiased:

```
useCache = useRtsl
        && pathDepth == 0
        && pathSplitIdx == 0
        && rtslCacheParams.enabled
        && !(NRC_UPDATE without NRC_QUERY-matching gate)   ← see below
```

When `useCache == false`, BOTH `lcSelectSubtreeRoot` AND `lcEvaluateMixturePdf` short-circuit BEFORE any `rng.nextFloat()` call: sampler returns root (0u), pdf returns `p_root` (which equals `evaluateLightSelectPdf` exactly). RNG sequence and result are byte-identical to the pre-cache code path.

**NRC consistency rule (CRITICAL).** `lightPdfRtsl` runs in both `NRC_UPDATE` and `NRC_QUERY` builds (`path_tracing.rgs.hlsl:525-527`). If UPDATE writes training samples using a different pdf than QUERY uses for MIS, NRC absorbs the bias. Therefore:
- The cache is either **on in BOTH UPDATE and QUERY** or **off in BOTH**.
- A single compile-time decision: gate on `rtslCacheParams.enabled` only; do NOT gate on `NRC_UPDATE`/`NRC_QUERY`.
- NRC_UPDATE's training-dim pixels map deterministically to render-dim pixels via `path_tracing.rgs.hlsl:547`, so the cache lookup at a training pixel is well-defined.
- Implication: under NRC_UPDATE, sparse training-dim pixels scatter their inserts across the screen tile grid. That's fine — the cache is still trained by QUERY-dispatched pixels at full screen density.

## Data layout

### Tile cache payload (double-buffered)

Slot packing: two `u32`s per slot. First u32 = `lightIdx`, second u32 = `normalOct` (16 bits used, 16 reserved). Total 8 B per slot.

Reasons:
- Atomic CAS targets only the lightIdx u32; reading the tag does NOT race because Prev is read-only this frame.
- 8 B keeps the slot 4-aligned for future packing (additional tag bits, generation stamp).
- `InterlockedCompareExchange` on the lightIdx u32 requires only 4-byte alignment of that u32. Satisfied.

Resources:
- `dev_rtslTileCacheA` / `dev_rtslTileCacheB` — `RWByteAddressBuffer`, each sized `tilesX * tilesY * SUB_BUCKETS * K_MAX * 8 bytes`. Ping-pong each frame.
- `dev_linearDepthPrev` — extra `R32_FLOAT` screen-sized texture; ping-ponged with `linearDepthTarget` each frame.
- No separate meta buffer — no eviction → no `lastInsertFrame`.
- No hash entries buffer — fixed grid indexed directly.

Sizing constants (`common_settings.h`):
- `RTSL_TILE_PIXELS = 8`
- `RTSL_TILE_SUB_BUCKETS = 4`
- `RTSL_LIGHT_CACHE_K_MAX = 32`

At 1920×1080: tilesX = 240, tilesY = 135 → 32,400 tiles × 4 × 32 = 4,147,200 slots × 8 B = 33.18 MB per buffer × 2 = **66.4 MB** total. `linearDepthPrev` adds ~8 MB.

At 3840×2160: ~266 MB. Acceptable on the GPUs this project targets.

### Resize handling

`MAX_RENDER_SIZE` does NOT exist in the codebase. Resize uses `RtTarget::setDimensions` + recreate (`renderer.cpp:200-204`) followed by full re-init of the affected resources. Match the same pattern for the tile cache + linearDepthPrev:

- Buffers live in `renderState` like other RtTargets.
- `resize()` flushes the queue (already does), reallocates the tile cache pair and `linearDepthPrev`, and re-registers SRV+UAV pairs in the descriptor heap.
- One-shot clear on (re)allocation initializes `lightIdx` slots to `LIGHT_IDX_INVALID`.

Plan **does not** chase the "allocation-free resize" property. Resize is already a flushing operation; one more allocation is free in context.

### Heap indices

Extend `HeapIndices`:
```cpp
struct HeapIndices {
    struct {
        // ... existing fields ...
        uint rtslTileCacheCurrIdx;       // UAV — written this frame
        uint linearDepthCurrIdx;         // UAV — written by gbuffer (rename existing linearDepthTargetIdx for clarity)
    } uav;
    struct {
        // ... existing fields ...
        uint rtslTileCachePrevSrvIdx;    // SRV — read this frame
        uint linearDepthPrevSrvIdx;      // SRV — read by tcLookupReprojected
    } srv;
};
```

`linearDepthCurr` ping-pongs with `linearDepthPrev` each frame at the heap-index level. The physical D3D12 resources also alternate roles. **DLSS-D tagging** (`renderer.cpp:485`) MUST follow the physical resource that holds THIS frame's depth (the new "Curr"), not a stale pointer. Plan step 6 makes this explicit.

## Sub-bucket key

```hlsl
uint tcDepthBucket(float linearDepth)
{
    // 4 log-spaced bands. Cover [near, far] roughly evenly in log2(1 + d).
    const float lg = log2(linearDepth + 1.0f) * rtslCacheParams.depthBucketScale;
    return clamp(uint(lg), 0u, RTSL_TILE_SUB_BUCKETS - 1u);
}

uint tcSubBucket(float linearDepth, float3 normal_WS)
{
    // Depth axis only. Normal goes into the per-slot tag, not the bucket key,
    // because normals rotate with the camera and would otherwise drop reproject
    // hit rate every camera turn.
    return tcDepthBucket(linearDepth);
}
```

`depthBucketScale` exposed as a setting; default tuned so 4 bands cover `[near, far]` evenly in log2 (one band per ~3 doublings).

Per-slot **normalTag** holds `octEncode(normal_WS) & 0xFFFF` (16 bits). On read, slots whose stored octahedral normal disagrees with the current pixel's normal beyond `rtslCacheRejectNormalCos` are treated as filled-but-unusable: they contribute to `K - numAccepted` in `uniformFrac'`, NOT to `pSubtreeSum`.

### Bucket-boundary depth jitter (known limit)

Depth that hovers near a sub-bucket boundary will flip the lookup target frame-to-frame even when the surface is stable. Cost: cache misses for that band. Not a correctness issue (`numAccepted = 0` → pure root). Mitigations deferred:
- Option A: ±1-band fallback on miss (look the adjacent bucket too).
- Option B: hysteresis (use the bucket the pixel had last frame if depth is within ε of a boundary).
Skip for v1; revisit if step 8 throughput results show the miss rate is large.

## Reprojection (read path)

```hlsl
struct TileCacheLookup {
    bool valid;
    uint slots[RTSL_LIGHT_CACHE_K_MAX];
    uint normalTags[RTSL_LIGHT_CACHE_K_MAX];
};

TileCacheLookup tcLookupReprojected(uint2 currPixel,
                                    float3 surfNor_WS,
                                    float currLinearDepth,
                                    ByteAddressBuffer cachePrev,
                                    Texture2D<float> linearDepthPrevTex,
                                    Texture2D<float2> motionTex)
{
    // First-frame / scene-cut / topology-change guard. cacheParams.suppressPrev
    // is set by the renderer in any of these cases — see "Scene-cut handling".
    if (renderParams.frameNumber == 0 || rtslCacheParams.suppressPrev != 0)
    {
        return tcLookupNull();
    }

    // motionTex is in UV space (gbuffer.rgs.hlsl :: calculateMotionFromPos).
    const float2 motion = motionTex[currPixel].xy;
    const float2 currUv = (float2(currPixel) + 0.5f) / float2(renderParams.renderSize);
    const float2 prevUv = currUv + motion;
    if (any(prevUv < 0.0f) || any(prevUv >= 1.0f))
    {
        return tcLookupNull();
    }

    // Sky/dome guard: a current pixel with linearDepth at farPlane has no
    // shading surface; the NEE call site won't fire, but keep the guard for
    // defense-in-depth.
    if (currLinearDepth >= cameraParams.farPlane * 0.99f)
    {
        return tcLookupNull();
    }

    const uint2 prevPixel = uint2(prevUv * float2(renderParams.renderSize));
    const float prevLinearDepth = linearDepthPrevTex[prevPixel];

    // Depth-relative disocclusion: rejects camera-past-disocclusion-edge AND
    // also rejects the "prev pixel was sky" case (prev depth = farPlane, big delta).
    const float depthRel = abs(currLinearDepth - prevLinearDepth) /
                           max(currLinearDepth, prevLinearDepth);
    if (depthRel > rtslCacheParams.rejectDepthRel)
    {
        return tcLookupNull();
    }

    const uint2 prevTile = prevPixel / RTSL_TILE_PIXELS;
    // Sub-bucket computed from CURRENT linearDepth. Insert and read at the
    // same site use the same currLinearDepth → symmetric. Adjacent-frame
    // depth drift across a band boundary degrades hit rate but is unbiased
    // (numAccepted == 0 → pure root on both sampler and pdf).
    const uint subBucket = tcSubBucket(currLinearDepth, surfNor_WS);

    return tcLoadSlot(cachePrev, prevTile, subBucket);
}
```

`tcLookupNull` returns a `TileCacheLookup` with `valid = false` and `slots[*] = LIGHT_IDX_INVALID`. Downstream `numAccepted` evaluates to 0; sampler returns root; pdf collapses to `p_root`.

## Insert path

Insert gate (identical to read gate, see "Gating discipline"). Insert site: inside the existing `if (lightSample.didHitLight)` branch in `path_tracing.rgs.hlsl`, **after** the `!isPassthrough` check (so `surfPos_WS` and `surfNor_WS` are the real-bounce values).

```hlsl
void tcInsert(uint2 currPixel,
              float linearDepth,
              float3 normal_WS,
              uint lightIdx,
              RWByteAddressBuffer cacheCurr,
              inout RandomNumberGenerator rng)
{
    // Defensive assert in debug builds: lightIdx must never equal the empty
    // sentinel (collides with LIGHT_IDX_INVALID == ~0u and would corrupt
    // dup-detection). Callers gate on lightSample.didHitLight which implies
    // a valid lightIdx, but keep the check.
    // (See note on sentinel collision below.)

    const uint2 tile = currPixel / RTSL_TILE_PIXELS;
    const uint subBucket = tcSubBucket(linearDepth, normal_WS);
    const uint slotBase = ((tile.y * tilesX + tile.x) * RTSL_TILE_SUB_BUCKETS + subBucket) * RTSL_LIGHT_CACHE_K_MAX;
    const uint normalOct16 = octEncode(normal_WS) & 0xFFFFu;

    // Pass 1: scan up to lightsPerCell slots for dup-or-empty.
    for (uint s = 0; s < rtslCacheParams.lightsPerCell; ++s)
    {
        const uint off = (slotBase + s) * 8u;
        uint existing;
        cacheCurr.InterlockedCompareExchange(off, LIGHT_IDX_INVALID, lightIdx, existing);
        if (existing == lightIdx) { cacheCurr.Store(off + 4u, normalOct16); return; }
        if (existing == LIGHT_IDX_INVALID) { cacheCurr.Store(off + 4u, normalOct16); return; }
    }

    // Pass 2: all K full, no match. Random-replace. RNG consumed ONLY on this branch.
    const uint subSlot = min(uint(rng.nextFloat() * float(rtslCacheParams.lightsPerCell)),
                             rtslCacheParams.lightsPerCell - 1u);
    const uint off = (slotBase + subSlot) * 8u;
    cacheCurr.Store(off,      lightIdx);
    cacheCurr.Store(off + 4u, normalOct16);
}
```

**Sentinel collision note.** `LIGHT_IDX_INVALID == LEAF_IDX_INVALID == ~0u` (`common_structs.h:70,177`). The empty-slot sentinel and the dropped-light sentinel use the same value; semantically distinct but symmetric ("fall back to root" in both cases), so the conflation is benign. Asserting `lightIdx != LIGHT_IDX_INVALID` in `tcInsert` prevents a real bug (storing the sentinel would block all future inserts to that slot).

## Read / pdf — shared accept predicate

To eliminate sampler-vs-pdf drift, both sites consume the same lookup result and the same accept predicate:

```hlsl
// Single source of truth for "is slot s usable as a cached subtree seed".
// Must be called with IDENTICAL arguments on the sampler and pdf side.
bool tcSlotAccepts(uint lightIdxStored, uint normalTagStored, float3 surfNor_WS)
{
    if (lightIdxStored == LIGHT_IDX_INVALID) return false;
    if (rtslLightToLeaf[lightIdxStored] == LEAF_IDX_INVALID) return false;
    return tcNormalTagAccepts(normalTagStored, surfNor_WS);
}
```

```hlsl
uint lcSelectSubtreeRoot(uint2 currPixel,
                         float3 surfNor_WS,
                         float currLinearDepth,
                         ByteAddressBuffer cachePrev,
                         Texture2D<float> linearDepthPrev,
                         Texture2D<float2> motion,
                         inout RandomNumberGenerator rng)
{
    // Byte-identical-when-disabled: short-circuit BEFORE any nextFloat() call.
    if (!rtslCacheParams.enabled || rtslParams.treeLeafCount == 0u) return 0u;

    const float coin = rng.nextFloat();
    if (coin < rtslCacheParams.uniformFrac) return 0u;

    const TileCacheLookup lk = tcLookupReprojected(
        currPixel, surfNor_WS, currLinearDepth, cachePrev, linearDepthPrev, motion);
    if (!lk.valid) return 0u;

    const uint subSlot = min(uint(rng.nextFloat() * float(rtslCacheParams.lightsPerCell)),
                             rtslCacheParams.lightsPerCell - 1u);
    if (!tcSlotAccepts(lk.slots[subSlot], lk.normalTags[subSlot], surfNor_WS)) return 0u;

    const uint leafIdx = rtslLightToLeaf[lk.slots[subSlot]];
    return lcAncestorAt(leafIdx, lcEffectiveLevels());
}

float lcEvaluateMixturePdf(uint areaLightIdx,
                           uint2 currPixel,
                           float3 surfPos_WS, float3 surfNor_WS,
                           float currLinearDepth,
                           ByteAddressBuffer cachePrev,
                           Texture2D<float> linearDepthPrev,
                           Texture2D<float2> motion)
{
    // Byte-identical-when-disabled.
    if (!rtslCacheParams.enabled || rtslParams.treeLeafCount == 0u)
        return evaluateLightSelectPdf(areaLightIdx, surfPos_WS, surfNor_WS);

    const float pRoot = evaluateLightSelectPdf(areaLightIdx, surfPos_WS, surfNor_WS);
    const TileCacheLookup lk = tcLookupReprojected(
        currPixel, surfNor_WS, currLinearDepth, cachePrev, linearDepthPrev, motion);

    const uint K = rtslCacheParams.lightsPerCell;

    if (!lk.valid)
    {
        // Same as numAccepted = 0 — uniformFrac' = 1, p_total = p_root.
        return pRoot;
    }

    // ---- Pdf-side hoist (PERF CRITICAL — see step 4) ----
    // All K slots share (surfPos, surfNor). Slots that share the same
    // lcAncestorAt(leafIdx, L) walk the same subtree. Group slots by their
    // subtree root and call lcEvaluateSubtreePdf ONCE per unique root,
    // weighting by group size. Naive K calls would be ~K× more expensive.
    float pSubtreeSum = 0.f;
    uint numAccepted = 0u;
    // pseudocode — see "Pdf hoist" below for the concrete grouping.
    for (uint s = 0u; s < K; ++s)
    {
        if (!tcSlotAccepts(lk.slots[s], lk.normalTags[s], surfNor_WS)) continue;
        ++numAccepted;
        const uint leafIdx = rtslLightToLeaf[lk.slots[s]];
        const uint subtreeRoot = lcAncestorAt(leafIdx, lcEffectiveLevels());
        // hoist: if subtreeRoot was already evaluated, reuse that pdf
        pSubtreeSum += lcEvaluateSubtreePdf(subtreeRoot, areaLightIdx, surfPos_WS, surfNor_WS);
    }

    const float uniformFracPrime = rtslCacheParams.uniformFrac
                                 + (1.f - rtslCacheParams.uniformFrac) * float(K - numAccepted) / float(K);
    return uniformFracPrime * pRoot + (1.f - rtslCacheParams.uniformFrac) * pSubtreeSum / float(K);
}
```

**`uniformFrac'` correctness** (validated by prior plan's Python emulator, ported in step 3):
```
uniformFrac' = uniformFrac + (1 - uniformFrac) · (K - numAccepted) / K
```
`numAccepted` counts slots that pass `tcSlotAccepts`. The `K - numAccepted` term covers empty + normal-rejected + dropped-light slots identically — all collapse to root on the sampler side, and all add to the root-weighted mass on the pdf side. Exact, not approximate.

`lightsPerCell` is the RUNTIME slot count (clamped to `K_MAX` at upload). Both sampler `subSlot = rng * lightsPerCell` and the pdf loop bound use the runtime value. **`K_MAX` is only the buffer-sizing constant.**

## Pdf hoist (was step 11 in prior plan — now step 4)

Per-NEE cost without the hoist: 1 root walk (~`logM` ≈ 12 nodes) + K subtree walks (`L` = 3 nodes each) = ~108 node loads. Per first-bounce-diffuse NEE plus per first-bounce-emission MIS = 2 calls/pixel. At 2M pixels × 60 fps = ~24G dependent loads/sec. Comparable to the entire light tree's current traversal budget. **This eats the 0.85× cache-on throughput threshold from step 8.5 if not hoisted.**

Hoist: group the K accepted slots by their `lcAncestorAt(leafIdx, L)` subtree root. Call `lcEvaluateSubtreePdf` once per UNIQUE subtree root, multiply by the count of slots sharing that root. Worst case unchanged (K unique roots) — best case (all K share one root, common in clustered scenes) drops to 1 walk.

Implementation: an HLSL-local fixed-size scratch array `uint scratchRoots[K_MAX]; uint scratchCounts[K_MAX]; uint nUnique = 0;`. Insertion-scan-and-increment is O(numAccepted × nUnique), which is fine at K_MAX = 32. Validate with `analyze_cache_hist.py` that the hoisted pdf matches the naive form to FP rounding.

## Settings

| Setting | Default | Range | Description |
|---|---|---|---|
| `rtslCacheEnabled` | true | bool | Master toggle |
| `rtslCacheLevels` | 5 | int [3, 7] | `L` — descent start level above cached leaf |
| `rtslCacheUniformFrac` | 0.20 | float [0.05, 1] | Mixture weight on root descent |
| `rtslCacheLightsPerCell` | 16 | int [1, 32] | Active slots per sub-bucket (clamped to `K_MAX`) |
| `rtslCacheRejectDepthRel` | 0.05 | float [0, 1] | Relative-depth disocclusion tolerance |
| `rtslCacheRejectNormalCos` | 0.7 | float [0, 1] | Min cosine for normal-tag acceptance |
| `rtslCacheDepthBucketScale` | 0.3333 | float | log2-depth → sub-bucket scaling (≈1 band per 3 doublings) |

All exposed via `SettingsManager`, GUI sliders, CLI. **All wire `didPathTracingSettingsChange`** so toggling resets accumulation AND `suppressPrev` triggers (see "Scene-cut handling").

## Scene-cut handling

`suppressPrev` is set ONLY for two conditions (v3 — narrowed from the v2 list after
implementation review):
- `frameNumber == 0` — Prev contents undefined.
- `didPathTracingSettingsChange` — already resets accumulation; cheap to also drop Prev for one frame.

```cpp
const bool suppressPrev =
    renderState.frameNumber == 0
    || renderState.didPathTracingSettingsChange;
rtslCacheParams->suppressPrev = suppressPrev ? 1u : 0u;
```

Mechanism: a per-frame `RtslCacheParams::suppressPrev` flag. `tcLookupReprojected`
checks it and short-circuits to `tcLookupNull`.

**Deliberately NOT suppressed** (v2 listed these; reviewed and dropped):
- **`globalInstanceOffset` rebase.** `worldToPrevClipMat` is patched for the FULL
  offset delta in `camera.cpp` `setMatrices` (the `globalInstanceOffsetChanged`
  branch, comment: "this one is necessary to fix motion vectors"). The v2 claim
  that "arbitrary chunk-coordinate jumps still produce bad motion vectors" was
  wrong — the correction is the full integer delta, not small-drift only. Screen
  is continuous and motion vectors stay correct across a rebase, so the
  screen-space cache reprojects fine. No suppress needed.
- **Light-tree topology change.** In voxel mode, chunk load/unload alters emitter
  topology almost every frame; suppressing on `didAreaLightTopologyChange()` would
  clear Prev continuously and the cache would never accumulate during movement —
  the dominant case. It is also not a correctness need: a removed light's cached
  slot self-invalidates via the `rtslLightToLeaf[lightIdx] == LEAF_IDX_INVALID`
  reject in `tcSlotAccepts`, and even if a sparse `areaLightIdx` is reused for a
  different light, the estimator stays unbiased because `lcSelectSubtreeRoot` and
  `lcEvaluateMixturePdf` consume the SAME lookup — a stale/reused index is at worst
  a poorer sampling seed (higher variance), never bias.

Camera teleport (the v2 `kTeleportThresholdSq` heuristic) is also dropped: per-pixel
disocclusion (motion vector + `rejectDepthRel`) already rejects bad reprojections,
and in voxel mode large discontinuous jumps go through `globalInstanceOffset` whose
motion vectors are corrected as above.

## Implementation steps

### Step 1 — Constants + struct + params plumbing

- Add `RTSL_TILE_PIXELS`, `RTSL_TILE_SUB_BUCKETS`, `RTSL_LIGHT_CACHE_K_MAX` to `common_settings.h`.
- Add `RtslCacheParams` struct to `common_params.h` (between `RtslParams` and `DebugParams`). Fields: `enabled`, `levels`, `uniformFrac`, `lightsPerCell`, `rejectDepthRel`, `rejectNormalCos`, `depthBucketScale`, `suppressPrev`, + pads to 16-byte align.
- Insert into `GlobalParams` cbuffer in `global_params.hlsli`.
- Register seven user-facing settings in `settings_manager.cpp` with defaults above. ALL wire `didPathTracingSettingsChange`.
- Per-frame upload in `renderer.cpp` clamps `lightsPerCell` to `RTSL_LIGHT_CACHE_K_MAX` (static_assert next to the clamp).
- Add the scene-cut detection logic (see "Scene-cut handling"). Sets `suppressPrev`.
- GUI section "RTSL Cache" under Sampling.
- No shader read sites yet. RTSL path byte-identical. Goldens `cave`, `cave_lights`, `cornell_box_rtsl`, `two_triangles` pass.

### Step 2 — GPU resources

- Add `dev_rtslTileCacheA` / `dev_rtslTileCacheB` `RWByteAddressBuffer`s, allocated at current render-size (NOT MAX_RENDER_SIZE — reallocate on resize).
- Add `dev_linearDepthPrev` matching `linearDepthTarget`'s format/dimensions. **Do NOT add to `autoTransitionRtTargets`** — manual transitions only, so PT can read it in `NON_PIXEL_SHADER_RESOURCE` instead of the autotransition's `PIXEL_SHADER_RESOURCE`. Same applies to `linearDepthTarget` if it gets renamed to `linearDepthCurr` — both Curr and Prev linear depth need manual transitions.
- Wire all three into the existing resize path (`renderer.cpp:200-204`).
- Register SRV + UAV pairs in the shared descriptor heap.
- Extend `HeapIndices` (see "Heap indices" above).
- Add `rtsl_tile_cache_clear.cs.hlsl` — each thread clears 16 slots (256k threads at 1080p, 1024 workgroups at WG=256). PSO + root sig (`GLOBAL_PARAMS` only, heap-direct-indexed).
- One-shot init clear writes `LIGHT_IDX_INVALID` to BOTH `tileCacheA` and `tileCacheB`. After init clear, transition one of them (call it "Prev for frame 0") to `NON_PIXEL_SHADER_RESOURCE`; the other stays UAV.
- `linearDepthPrev` initial state: contents undefined on frame 0, but `suppressPrev=1` on frame 0 means `tcLookupReprojected` never reads it. Initial state = `NON_PIXEL_SHADER_RESOURCE` (matching its role as Prev for frame 1's read).
- Buffers allocated but not yet used by any shader. Goldens pass.

### Step 3 — Write `tile_cache.hlsli`

- New folder `src/shaders/tile_cache/`.
- Public symbols: `tcInsert`, `tcLookupReprojected`, `tcEvaluateMixturePdf`, `tcEvaluateSubtreePdf` (reused from prior plan's design), `tcSubBucket`, `tcDepthBucket`, `tcNormalTagAccepts`, `tcSlotAccepts`, `lcAncestorAt`, `lcEffectiveLevels`, `lcSelectSubtreeRoot`. Helper: `tcLookupNull`, `tcLoadSlot`.
- Root-relative includes (`-I src/shaders/` already enabled per `light_cache.hlsli` precedent in prior plan): `common/global_params.hlsli`, `util/packing.hlsli` (for `octEncode` / `octDecode`), `util/rng.hlsli`.
- Port prior plan's `plans/test_rtsl_cache_mis.py` MIS validator to screen-space. Configurations: disabled, empty bucket, partial, full, normal-rejected mix, disoccluded, dropped-light mix, K=K_MAX dense, L=lcEffectiveLevels collapsing to root. Pass: max z < 5.0 across all configs at 2M samples. Negative control: `/numAccepted` form must trip the bias detector. Output: `rtsl_cache/test_cache_mis.py`.
- Header is unused by any other shader at this step. Goldens unaffected.

### Step 4 — Modify `sampleDirectLightingRtsl` and `lightPdfRtsl`

- Add new params to both signatures: `currPixel`, `currLinearDepth`, `cachePrev`, `linearDepthPrev`, `motionTex`. Buffers fetched at the call sites in `path_tracing.rgs.hlsl` via bindless heap.
- `sampleDirectLightingRtsl`: replace `selectLightFromSubtree(0u, ...)` with `selectLightFromSubtree(lcSelectSubtreeRoot(...), ...)`. Replace `result.pdfOrW_Y = pdfSelect * lightSamplePdf` with `result.pdfOrW_Y = lcEvaluateMixturePdf(pickedLightIdx, ...) * lightSamplePdf`.
- `lightPdfRtsl`: replace `evaluateLightSelectPdf(areaLightIdx, ...)` with `lcEvaluateMixturePdf(areaLightIdx, ...)`. Area-to-solid-angle conversion unchanged.
- Include `tile_cache/tile_cache.hlsli` in `light_tree_sampling.hlsli` between `evaluateLightSelectPdf` and `lightPdfRtsl`.
- **Implement the pdf hoist in this step** (not deferred). Validate the hoisted result equals the naive K-walk form to FP rounding on a 1024-sample synthetic test in `rtsl_cache/test_pdf_hoist.py`.
- RNG byte-identical when disabled: early-out in `lcSelectSubtreeRoot` before any `nextFloat()`. `lcEvaluateMixturePdf` consumes no RNG either way.
- **Gate consistency.** `lightPdfRtsl` is called in BOTH first-bounce and deeper bounces (per the path tracer's loop structure at `path_tracing.rgs.hlsl:523-528`). But the read-side cache lookup should fire only at `pathDepth == 0`. Solution: `lcEvaluateMixturePdf` early-outs to `evaluateLightSelectPdf` if `currPixel` is signaled "not at depth 0" — pass a sentinel like `uint2(0xFFFFFFFFu, 0xFFFFFFFFu)` from the depth>0 call sites. Cleaner alternative: keep `lightPdfRtsl` cache-aware only at depth 0; introduce a non-cache `lightPdfRtslUniform` variant for the depth>0 / passthrough-recovery callers. **Pick the explicit-variant approach** — avoids a magic-pixel sentinel and lets the compiler see the two paths cleanly.
- Confirm with `cave`, `cave_lights`, `cornell_box_rtsl`, `two_triangles`.

### Step 5 — Inline insert at NEE-hit branch in `path_tracing.rgs.hlsl`

- Insert site: inside existing `if (lightSample.didHitLight)` block in `path_tracing.rgs.hlsl` (around line 305), BEFORE BSDF/MIS contribution code.
- Insert gate: `useRtsl && pathDepth == 0 && pathSplitIdx == 0 && rtslCacheParams.enabled`. (No `NRC_UPDATE` gate per "NRC consistency rule" above.)
- **Passthrough handling.** The insert is already inside the `else // !isPassthrough` block (the NEE branch at `path_tracing.rgs.hlsl:270` only runs there). `surfPos_WS` / `surfNor_WS` are the real-bounce values. For path-tracing-loop iterations where the *first* bounce was a passthrough (rare with primary-ray gbuffer driving it, but possible if `refractionIndirectPassthrough` is on and the gbuffer hit a transmissive delta surface): `pathDepth == 0` could fire on a non-passthrough hit but the underlying surface seen by the camera is glass. `pixelIdx` (= camera ray pixel) keys the screen tile to the GLASS surface, not the surface behind it. Mitigations:
  - Option A: also gate insert on `!bool(payload.flags & PAYLOAD_FLAG_FIRST_HIT_WAS_PASSTHROUGH)` — needs a new payload flag set by the path-tracer entry.
  - Option B: accept the lost coverage (the glass tile collects lights for the surface behind, which is geometrically close anyway).
  - Pick Option B for v1; revisit if step 8 validation shows artifacts at glass surfaces.
- Fetch `tileCacheCurr` UAV inline via `ResourceDescriptorHeap[heapIndices.uav.rtslTileCacheCurrIdx]`.
- Call `tcInsert(pixelIdx, currLinearDepth, surfNor_WS, lightSample.lightIdx, tileCacheCurr, payload.rng)`.
- All 48 goldens pass at `rtslCacheEnabled=false` (RNG byte-identical when cache off).

### Step 6 — Wire dispatches + state transitions in `renderer.cpp` render loop

Per-frame order (mandatory):

```
1. G-buffer pass                                  [writes linearDepthCurr UAV, motionTarget UAV, normalsAndRoughness UAV]
2. Compute scene-cut conditions; set suppressPrev [CPU]
3. State transition table (BEFORE clear):
   - tileCacheCurr:    NON_PIXEL_SHADER_RESOURCE → UAV   (was Prev last frame)
   - tileCachePrev:    UAV → NON_PIXEL_SHADER_RESOURCE   (was Curr last frame)
   - linearDepthCurr:  UAV → NON_PIXEL_SHADER_RESOURCE   (gbuffer just wrote)
   - linearDepthPrev:  whatever state ↔ NON_PIXEL_SHADER_RESOURCE for PT read
   - motionTarget:     UAV → NON_PIXEL_SHADER_RESOURCE   (gbuffer just wrote)
4. tile_cache_clear dispatch                      [writes tileCacheCurr UAV]
5. UAV barrier on tileCacheCurr                   [clear → PT write race-prevention]
6. Path tracing dispatch                          [reads tileCachePrev SRV + linearDepthPrev SRV + motionTarget SRV;
                                                   writes tileCacheCurr UAV; linearDepth* and motion are read NPSR]
7. End-of-frame ping-pong:
   - Swap heap indices for tileCache{A,B}
   - Swap heap indices for linearDepth{Curr,Prev}
   (Physical state transitions for the swap happen at the start of NEXT frame, step 3.)
```

Critical points:
- **DLSS-D tagging** at `renderer.cpp:485` must take `linearDepthCurr` (the one gbuffer just wrote) AFTER step 7's ping-pong has NOT yet happened, OR the tagging uses the resource that's about to become Prev. Simplest: tag DLSS before step 7. Verify by running existing DLSS goldens.
- The previous-frame `linearDepth` transition (UAV → NPSR) is critical because gbuffer writes it as UAV but PT (raygen) reads it as NPSR. Existing `autoTransitionRtTargets` does this for the OTHER targets but pushes them to `PIXEL_SHADER_RESOURCE`; we want NPSR, so we manage `linearDepthCurr` / `linearDepthPrev` manually and remove them from `autoTransitionRtTargets`. Double-check that nothing else downstream (denoiser, post) reads them — if so, those readers either accept NPSR or get an extra explicit transition.
- First-frame `suppressPrev = 1` means PT never dereferences `tileCachePrev` or `linearDepthPrev` on frame 0; their contents are undefined except the one-shot init clear of the tile cache.

### Step 7 — DLSS / antialiasing interaction

- DLSS jitter is in `cameraParams.jitter` (UV). Motion vector from `gbuffer.rgs.hlsl :: calculateMotionFromPos` uses `worldToClipMat` / `worldToPrevClipMat`, both jitter-aware. Reprojection at a jittered pixel lands at the corresponding prev-jittered pixel — correct for cache hit-rate.
- Tile size 8 vs. typical jitter < 1 pixel: jitter cannot push a pixel out of its tile, so cache locality holds.
- Test path: `cornell_box_rtsl_cache_uniform0` + `cave_rtsl_cache_uniform0` (both DLSS-on configurations).

### Step 8 — Validation

Each item is a single numeric pass/fail.

1. **Existing goldens pass byte-identically with cache off.** `rtslCacheEnabled=false`. Pass: zero RMSE regression on the 48-entry suite vs. `main`.

2. **New cache-on goldens** added to `tests/tests.json`:
   - `cornell_box_rtsl_cache_uniform0` — `--samplingMode=3 --rtslCacheEnabled=true --rtslCacheUniformFrac=0.0 --rtslCacheLevels=3 --rtslCacheLightsPerCell=4`. Threshold 0.015.
   - `cornell_box_rtsl_cache_uniform1` — same scene, `--rtslCacheUniformFrac=1.0`. Threshold 0.015. Matches cache-off modulo RNG-sequence shift on the Pass 2 random-replace branch.
   - `cornell_box_rtsl_cache_L2K1` — `--rtslCacheLevels=2 --rtslCacheLightsPerCell=1`. Threshold 0.015.
   - `cave_rtsl_cache_uniform0` — cave scene, `--rtslCacheUniformFrac=0.0 --maxAccumulatedFrames=4096`. Threshold 0.015. **Strongest pure-cache bias detector.**
   - `cornell_box_rtsl_cache_static_vs_moving` — static-camera 1024-spp baseline vs. camera-spin-then-settle (covers reprojection / disocclusion correctness). Threshold 0.02.
   - `cornell_box_rtsl_cache_mirror_floor` — cornell box with a fully reflective floor (audit-suggested). Verifies that delta-surface tiles don't trip the cache lookup. Threshold 0.015.
   - `nrc_consistency_cache_on_vs_off` — NRC_QUERY-mode render, cache on AND cache off, expect both to converge to the same target since NRC is trained against the same MIS pdf in both states. Threshold 0.015.

   Pass: all entries under threshold.

3. **Shader-side empirical-vs-pdf histogram** (one-shot, scripted). `--debugRtslCacheStats=<path>` debug feature; bind buffer `dev_dbgRtslSamples`; write `(pixelIdx, tileIdx, subBucket, sampledLightIdx, p_total)` for first-bounce NEE samples on the final accumulated frame. Python analyzer `rtsl_cache/analyze_cache_hist.py` groups by `(tileIdx, subBucket)`, builds empirical histograms, computes z-scores. Pass: max z < 5.0 across all `(tile, bucket, light)` bins with count ≥ 20.

4. **Sub-slot RNG uniformity.** Same test as prior plan's `test_rtsl_subslot_rng.py`. Pass: bucket index `K` never produced + uniformity bound met.

5. **Throughput regression.** `cave` with `--lockCamera`, cache off vs. on, 5 wall-clock seconds each. Pass: cache-off SPP/sec ≥ 0.95 × pre-feature baseline; cache-on ≥ 0.85 × cache-off. **If this fails, the pdf hoist (step 4) is likely the leverage point — profile it first.**

6. **MIS Python simulator.** `rtsl_cache/test_cache_mis.py`. Pass: max z < 5.0 across all configs; `/numAccepted` negative control trips.

7. **Reprojection disocclusion correctness.** Synthetic test in `rtsl_cache/test_disocclusion.py`: feed in a depth discontinuity, check classification. Pass: 100% accuracy.

8. **Pdf-hoist equivalence.** `rtsl_cache/test_pdf_hoist.py`. Naive K-walk pdf vs. hoisted pdf on 1024 random tile configurations. Pass: max relative error < 1e-5.

9. **Light-tree-rebuild regression.** Scripted scene with mid-accumulation light add/remove. Verify `suppressPrev` fires on the rebuild frame and accumulation does not produce visible artifacts. Pass: per-frame RMSE on the rebuild frame within 2× the steady-state frame RMSE.

## Risks and open questions (post-audit)

- **Scene-cut detector threshold.** `kTeleportThresholdSq` and `kOffsetThreshold` need numeric values from cave-walk profiling. Lean: 1m camera jump or any non-zero offset delta triggers suppress. Tune in implementation.
- **Bucket-boundary hysteresis** (deferred). Step 8.5 throughput data tells us if it's worth implementing.
- **Mirror-floor cache lookup** (covered by goldens 2.6).
- **Random-replace churn at locked-camera accumulation.** RNG in Pass 2 is consumed only on full-bucket inserts; for accumulation-mode locked cameras, the bucket converges to a stable set after `O(K)` frames and Pass 2 stops firing. Cheap to verify empirically with the histogram test (step 8.3).

## Out of scope (deferred to follow-ups)

- **Indirect-bounce caching.** NRC absorbs the indirect domain. Revisit if NRC quality bottlenecks on direct-lighting noise specifically.
- **BSDF-hit emission inserts at depth 0.** Free signal but adds insert sites at the emitter pixel (not the surface-bouncing-off-it pixel) — distinct from NEE insert. Defer until first-bounce cache wins are measured.
- **Adaptive sub-bucket scaling.**
- **Reservoir-style temporal accumulation.**
- **±1-band depth-bucket fallback / hysteresis.**

## File / dir layout

- New folder: `src/shaders/tile_cache/`
- New shaders: `tile_cache/tile_cache.hlsli`, `tile_cache/tile_cache_clear.cs.hlsl`
- Touched: `common_settings.h`, `common_params.h`, `common_registers.h` (tile cache + linear depth prev register slots), `global_params.hlsli`, `light_tree_sampling.hlsli`, `path_tracing.rgs.hlsl`, `gbuffer.rgs.hlsl` (no change needed; motion + linear depth already written), `renderer.cpp` (dispatch wiring + ping-pong + heap-index population + state transitions + DLSS tagging update), `renderer_init.cpp` (resource creation; **do not** push linearDepth* into `autoTransitionRtTargets`), `renderer_state.cpp` (named-RtTarget registration if `linearDepthPrev` is added to that map), `settings_manager.cpp`, `tests/tests.json`.
- Python: `rtsl_cache/test_cache_mis.py`, `rtsl_cache/test_pdf_hoist.py`, `rtsl_cache/analyze_cache_hist.py`, `rtsl_cache/test_disocclusion.py`, `rtsl_cache/findings.md` (perf baselines).

## Commit style

Subject line + 1-2 sentences. Details belong in this plan, not commit bodies.

---

## Appendix: v1 → v2 deltas

Triage of three parallel audits (MIS+patent, perf+dispatch, edge cases).

**[CRITICAL] resolved inline:**
1. NRC pdf consistency — cache must be on/off in BOTH `NRC_UPDATE` and `NRC_QUERY`, never mixed. Gate on `rtslCacheParams.enabled` only.
2. `MAX_RENDER_SIZE` invented — dropped. Reallocate on resize via the existing `RtTarget::setDimensions` pattern.
3. Mixture pdf gating at depth > 0 — introduce `lightPdfRtslUniform` variant for depth>0 callers; `lightPdfRtsl` (cache-aware) only called at depth 0.
4. Pdf hoist moved from "step 11" to "step 4" — perf-critical for hitting the 0.85× throughput threshold.
5. Explicit state-transition table in step 6.
6. `linearDepthPrev` pulled out of `autoTransitionRtTargets`; manual NPSR transitions only.
7. Light-tree topology change force-clears Prev via `suppressPrev`.
8. Scene-cut detector via `suppressPrev` flag covering: frame 0, camera teleport, `globalInstanceOffset` jump, topology change, settings change.
9. Shared `tcSlotAccepts(lightIdx, normalTag, surfNor_WS)` predicate as single source of truth for sampler ↔ pdf accept agreement.
10. Misleading "same sub-bucket" comment deleted; depth-band-boundary jitter explicitly acknowledged as a hit-rate (not correctness) issue.
11. `LIGHT_IDX_INVALID == LEAF_IDX_INVALID == ~0u` collision documented as intentional; `tcInsert` asserts `lightIdx != LIGHT_IDX_INVALID`.

**[LIKELY-BUG] resolved inline:**
- Passthrough first-hit cache pixel mismatch — explicitly call out as Option B (accept), revisit if validation flags artifacts.
- Mirror prev-pixel — golden test `cornell_box_rtsl_cache_mirror_floor` added.
- DLSS-D tagging follows physical Curr resource (renderer.cpp:485) — called out in step 6.

**[WORTH-CHECKING] resolved or noted:**
- `pathSplitIdx == 0` already covered by the shared gating discipline.
- Locked-camera random-replace churn — covered by step 8.3 histogram test; revisit if churn shows up.
- Bucket-boundary hysteresis — out of scope for v1.
- DLSS jitter < 1 pixel claim verified (tiles are 8 pixels).

**[NIT] resolved:**
- Clear pass: each thread clears 16 slots, not 1 (~50-100 μs actual cost).
- Pdf loop bound uses `lightsPerCell` not `K_MAX` — made explicit in step 3.
- `K_MAX` is buffer-sizing-only; runtime uses `lightsPerCell`.
