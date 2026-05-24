// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

// RTSL tile cache carry pass (weighted_plan.md step W2). Replaces the per-frame
// clear: instead of zeroing Curr, it reprojects the Prev cell into Curr and
// decays the visibility counters, so light membership and per-light statistics
// persist across frames. Path tracing then upserts this-frame outcomes on top.
//
// One thread per Curr cell (tile x sub-bucket). Reprojection is tile-center
// (one motion vector + one disocclusion test per tile, applied to all four
// sub-buckets) — coarse for silhouette tiles but cheap; see weighted_plan.md
// "Per-sub-bucket reprojection (planned refinement)".

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"

// Dead-light drop: a light removed from the tree (or whose sparse index was
// reused for a different light) must not keep squatting a carried slot for the
// whole decay window. Bound as a root SRV exactly like path tracing — it is the
// one resource the carry needs that does not live in the descriptor heap.
StructuredBuffer<uint> rtslLightToLeaf : REGISTER_T(LIGHT_TREE, LIGHT_TO_LEAF_IN);

#include "tile_cache/tile_cache_cells.hlsli"

// Writes a fully empty slot (the same sentinel the clear/init wrote).
static void writeEmptySlot(RWByteAddressBuffer cacheCurr, uint dstOff)
{
    cacheCurr.Store(dstOff, LIGHT_IDX_INVALID);
    cacheCurr.Store(dstOff + 4u, 0u);
    cacheCurr.Store(dstOff + 8u, 0u);
    cacheCurr.Store(dstOff + 12u, 0u);
}

[shader("compute")]
[numthreads(RTSL_TILE_CACHE_CARRY_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint tilesX = tcTilesX();
    const uint tilesY = (renderParams.renderSize.y + RTSL_TILE_PIXELS - 1u) / RTSL_TILE_PIXELS;
    const uint numCells = tilesX * tilesY * RTSL_TILE_SUB_BUCKETS;

    const uint cellIdx = dispatchThreadId.x;
    if (cellIdx >= numCells)
    {
        return;
    }

    const uint subBucket = cellIdx % RTSL_TILE_SUB_BUCKETS;
    const uint tileLinear = cellIdx / RTSL_TILE_SUB_BUCKETS;
    const uint2 tile = uint2(tileLinear % tilesX, tileLinear / tilesX);

    RWByteAddressBuffer cacheCurr = ResourceDescriptorHeap[heapIndices.uav.rtslTileCacheCurrIdx];
    const uint dstBase = tcSlotBase(tile, subBucket);

    // ---- Tile-center reprojection + disocclusion (mirrors the read path) ----
    // Reject (empty the whole cell) on: suppressPrev, frame 0, prevUv out of
    // bounds, or a failed center-depth disocclusion test.
    bool reject = (rtslCacheParams.suppressPrev != 0u) || (renderParams.frameNumber == 0u);
    uint2 prevTile = tile;

    if (!reject)
    {
        Texture2D<float2> motionTex = ResourceDescriptorHeap[heapIndices.srv.motionTargetIdx];
        Texture2D<float> linearDepthCurrTex = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
        Texture2D<float> linearDepthPrevTex = ResourceDescriptorHeap[heapIndices.srv.linearDepthPrevSrvIdx];

        const uint2 centerPixel = tile * RTSL_TILE_PIXELS + (RTSL_TILE_PIXELS / 2u);
        const float2 centerUv = (float2(centerPixel) + 0.5f) / float2(renderParams.renderSize);
        const float2 motion = motionTex[centerPixel].xy;
        const float2 prevUv = centerUv + motion;

        if (any(prevUv < 0.0f) || any(prevUv >= 1.0f))
        {
            reject = true;
        }
        else
        {
            const float currDepth = linearDepthCurrTex[centerPixel];
            const uint2 prevPixel = uint2(prevUv * float2(renderParams.renderSize));
            const float prevDepth = linearDepthPrevTex[prevPixel];
            if (tcDepthRejects(currDepth, prevDepth))
            {
                reject = true;
            }
            else
            {
                prevTile = prevPixel / RTSL_TILE_PIXELS;
            }
        }
    }

    if (reject)
    {
        [unroll]
        for (uint s = 0u; s < RTSL_LIGHT_CACHE_K_MAX; ++s)
        {
            writeEmptySlot(cacheCurr, (dstBase + s) * RTSL_TILE_CACHE_SLOT_BYTES);
        }
        return;
    }

    // ---- Copy the reprojected Prev cell, decaying counters and dropping dead
    // lights. Same sub-bucket index, matching the read convention. ----
    ByteAddressBuffer cachePrev = ResourceDescriptorHeap[heapIndices.srv.rtslTileCachePrevSrvIdx];
    const uint srcBase = tcSlotBase(prevTile, subBucket);

    [loop]
    for (uint s = 0u; s < RTSL_LIGHT_CACHE_K_MAX; ++s)
    {
        const uint srcOff = (srcBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
        const uint dstOff = (dstBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;

        const uint lightIdx = cachePrev.Load(srcOff);

        // Same dead-light predicate as tcSlotAccepts (sans the normal gate, which
        // carry has no current pixel for). HLSL 2021 short-circuits the &&, so the
        // rtslLightToLeaf index is never evaluated for the INVALID sentinel.
        if (lightIdx == LIGHT_IDX_INVALID || rtslLightToLeaf[lightIdx] == LEAF_IDX_INVALID)
        {
            writeEmptySlot(cacheCurr, dstOff);
            continue;
        }

        const uint normalTag = cachePrev.Load(srcOff + 4u);
        const uint attempts = cachePrev.Load(srcOff + 8u);
        const uint successes = cachePrev.Load(srcOff + 12u);

        cacheCurr.Store(dstOff, lightIdx);
        cacheCurr.Store(dstOff + 4u, normalTag);
        cacheCurr.Store(dstOff + 8u, tcPackCounter(float(attempts) * rtslCacheParams.statDecay));
        cacheCurr.Store(dstOff + 12u, tcPackCounter(float(successes) * rtslCacheParams.statDecay));
    }
}
