// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// RTSL screen-space tile light cache: read (reproject), insert, and the
// mixture sampler + MIS pdf that splices a cached-subtree descent into the
// per-pixel root descent of light_tree_sampling.hlsli.
//
// INCLUDE ORDER: this header is textually included partway through
// light_tree_sampling.hlsli (between evaluateLightSelectPdf and lightPdfRtsl,
// see step 4). It relies on symbols defined ABOVE that point:
//   rtslLightTree, rtslLightToLeaf      (buffers)
//   rtslParams                          (treeLeafBase / treeLeafCount)
//   rtslChildProbs                      (per-node descent split)
//   evaluateLightSelectPdf              (full root-descent pdf)
// It does NOT include light_tree_sampling.hlsli (circular). The includes below
// (pragma-once guarded) only pull in what is otherwise independent.

#include "../rendering/common/common_settings.h"
#include "common/global_params.hlsli"
#include "util/packing.hlsli"
#include "util/rng.hlsli"

// =============================================
// Subtree / level helpers
// =============================================

// HIS descent starts L levels above a cached leaf. Clamp L to the tree height
// (logM) so the subtree root never walks above the actual root: at L == logM
// the "subtree root" IS node 0 and the mixture collapses to pure root descent
// (the cache contributes nothing, but stays unbiased).
uint lcEffectiveLevels()
{
    const uint logM = firstbithigh(rtslParams.treeLeafCount);
    return min(rtslCacheParams.levels, logM);
}

// Ancestor of a perfect-binary-tree node `levels` steps toward the root.
// Index algebra on the (node+1) heap form: parent(i) = (i+1)/2 - 1, iterated.
uint lcAncestorAt(uint nodeIdx, uint levels)
{
    return ((nodeIdx + 1u) >> levels) - 1u;
}

// =============================================
// Sub-bucket key (depth only — see plan "Sub-bucket key")
// =============================================

uint tcDepthBucket(float linearDepth)
{
    const float lg = log2(linearDepth + 1.0f) * rtslCacheParams.depthBucketScale;
    return clamp(uint(lg), 0u, RTSL_TILE_SUB_BUCKETS - 1u);
}

// Normal is intentionally NOT part of the bucket key (it rotates with the
// camera and would tank reproject hit rate on every turn); it lives in the
// per-slot tag instead and gates acceptance via tcNormalTagAccepts.
uint tcSubBucket(float linearDepth, float3 normal_WS)
{
    return tcDepthBucket(linearDepth);
}

// =============================================
// Per-slot accept predicate — single source of truth (sampler == pdf)
// =============================================

// The slot tag word holds octEncode(normal_WS) (full 32-bit oct, not the
// 16-bit mask the plan sketched: masking drops the y component and the decode
// is unrecoverable; the tag word is a full u32 so we use all of it).
bool tcNormalTagAccepts(uint normalTagStored, float3 surfNor_WS)
{
    const float3 storedNor = octDecode(normalTagStored);
    return dot(storedNor, surfNor_WS) >= rtslCacheParams.rejectNormalCos;
}

// "Is slot s usable as a cached subtree seed?" Both the sampler and the pdf
// MUST call this with identical arguments so their accept sets never diverge —
// any divergence biases the MIS estimator.
bool tcSlotAccepts(uint lightIdxStored, uint normalTagStored, float3 surfNor_WS)
{
    if (lightIdxStored == LIGHT_IDX_INVALID)
    {
        return false;
    }
    if (rtslLightToLeaf[lightIdxStored] == LEAF_IDX_INVALID)
    {
        return false;
    }
    return tcNormalTagAccepts(normalTagStored, surfNor_WS);
}

// =============================================
// Lookup result + buffer load
// =============================================

struct TileCacheLookup
{
    bool valid;
    uint slots[RTSL_LIGHT_CACHE_K_MAX];
    uint normalTags[RTSL_LIGHT_CACHE_K_MAX];
};

TileCacheLookup tcLookupNull()
{
    TileCacheLookup lk;
    lk.valid = false;
    [unroll]
    for (uint s = 0u; s < RTSL_LIGHT_CACHE_K_MAX; ++s)
    {
        lk.slots[s] = LIGHT_IDX_INVALID;
        lk.normalTags[s] = 0u;
    }
    return lk;
}

uint tcTilesX()
{
    return (renderParams.renderSize.x + RTSL_TILE_PIXELS - 1u) / RTSL_TILE_PIXELS;
}

uint tcSlotBase(uint2 tile, uint subBucket)
{
    const uint tileLinear = tile.y * tcTilesX() + tile.x;
    return (tileLinear * RTSL_TILE_SUB_BUCKETS + subBucket) * RTSL_LIGHT_CACHE_K_MAX;
}

// Loads the active slots of one (tile, sub-bucket) cell from a read-only Prev
// buffer. Reads K_MAX entries (compile-time loop bound); slots beyond
// lightsPerCell stay INVALID from the frame's clear and are never accepted.
TileCacheLookup tcLoadSlot(ByteAddressBuffer cachePrev, uint2 tile, uint subBucket)
{
    TileCacheLookup lk;
    lk.valid = true;
    const uint slotBase = tcSlotBase(tile, subBucket);
    [unroll]
    for (uint s = 0u; s < RTSL_LIGHT_CACHE_K_MAX; ++s)
    {
        const uint off = (slotBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
        lk.slots[s] = cachePrev.Load(off);
        lk.normalTags[s] = cachePrev.Load(off + 4u);
    }
    return lk;
}

// =============================================
// Read path — motion-vector reprojection + disocclusion
// =============================================

TileCacheLookup tcLookupReprojected(uint2 currPixel,
                                    float3 surfNor_WS,
                                    float currLinearDepth,
                                    ByteAddressBuffer cachePrev,
                                    Texture2D<float> linearDepthPrevTex,
                                    Texture2D<float2> motionTex)
{
    // First-frame / scene-cut / settings-change guard (see "Scene-cut handling").
    if (renderParams.frameNumber == 0u || rtslCacheParams.suppressPrev != 0u)
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

    // Sky/dome guard: a pixel at the far plane has no shading surface. The NEE
    // call site won't fire there, but keep the guard for defense-in-depth.
    if (currLinearDepth >= cameraParams.farPlane * 0.99f)
    {
        return tcLookupNull();
    }

    const uint2 prevPixel = uint2(prevUv * float2(renderParams.renderSize));
    const float prevLinearDepth = linearDepthPrevTex[prevPixel];

    // Relative-depth disocclusion: rejects past-an-edge reprojections AND the
    // "prev pixel was sky" case (prev depth ~ farPlane → large delta).
    const float depthRel = abs(currLinearDepth - prevLinearDepth) /
                           max(currLinearDepth, prevLinearDepth);
    if (depthRel > rtslCacheParams.rejectDepthRel)
    {
        return tcLookupNull();
    }

    const uint2 prevTile = prevPixel / RTSL_TILE_PIXELS;
    // Sub-bucket from CURRENT depth so insert and read at the same site agree.
    // Adjacent-frame drift across a band boundary only costs hit rate, never
    // bias (numAccepted == 0 → pure root on both sampler and pdf).
    const uint subBucket = tcSubBucket(currLinearDepth, surfNor_WS);
    return tcLoadSlot(cachePrev, prevTile, subBucket);
}

// =============================================
// Insert path (write to this frame's Curr buffer)
// =============================================

void tcInsert(uint2 currPixel,
              float linearDepth,
              float3 normal_WS,
              uint lightIdx,
              RWByteAddressBuffer cacheCurr,
              inout RandomNumberGenerator rng)
{
    // Storing the empty sentinel would permanently block this slot's
    // dup/empty scan. Callers gate on lightSample.didHitLight (valid lightIdx),
    // but bail defensively rather than corrupt the cell.
    if (lightIdx == LIGHT_IDX_INVALID)
    {
        return;
    }

    const uint2 tile = currPixel / RTSL_TILE_PIXELS;
    const uint subBucket = tcSubBucket(linearDepth, normal_WS);
    const uint slotBase = tcSlotBase(tile, subBucket);
    const uint normalTag = octEncode(normal_WS);
    const uint K = rtslCacheParams.lightsPerCell;

    // Pass 1: scan for a duplicate or an empty slot via CAS on the lightIdx word.
    // Prev is read-only this frame, so reading the tag word never races.
    for (uint s = 0u; s < K; ++s)
    {
        const uint off = (slotBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
        uint existing;
        cacheCurr.InterlockedCompareExchange(off, LIGHT_IDX_INVALID, lightIdx, existing);
        if (existing == lightIdx || existing == LIGHT_IDX_INVALID)
        {
            cacheCurr.Store(off + 4u, normalTag);
            return;
        }
    }

    // Pass 2: cell full, no match. Random-replace. RNG consumed ONLY here, so
    // the disabled/empty/non-full paths stay RNG byte-identical to pre-cache.
    const uint subSlot = min(uint(rng.nextFloat() * float(K)), K - 1u);
    const uint off = (slotBase + subSlot) * RTSL_TILE_CACHE_SLOT_BYTES;
    cacheCurr.Store(off, lightIdx);
    cacheCurr.Store(off + 4u, normalTag);
}

// =============================================
// Mixture sampler — pick a subtree root for selectLightFromSubtree
// =============================================

// Returns the subtree root the forward HIS descent should start from. 0u
// (tree root) means "no cache contribution this sample". The caller feeds the
// result straight into selectLightFromSubtree.
//
// Byte-identical-when-disabled: every early-out below precedes any nextFloat()
// call, so with the cache off the RNG sequence matches the pre-cache path.
uint lcSelectSubtreeRoot(uint2 currPixel,
                         float3 surfNor_WS,
                         float currLinearDepth,
                         ByteAddressBuffer cachePrev,
                         Texture2D<float> linearDepthPrevTex,
                         Texture2D<float2> motionTex,
                         inout RandomNumberGenerator rng)
{
    if (rtslCacheParams.enabled == 0u || rtslParams.treeLeafCount == 0u)
    {
        return 0u;
    }

    const float coin = rng.nextFloat();
    if (coin < rtslCacheParams.uniformFrac)
    {
        return 0u;
    }

    const TileCacheLookup lk = tcLookupReprojected(
        currPixel, surfNor_WS, currLinearDepth, cachePrev, linearDepthPrevTex, motionTex);
    if (!lk.valid)
    {
        return 0u;
    }

    const uint K = rtslCacheParams.lightsPerCell;
    const uint subSlot = min(uint(rng.nextFloat() * float(K)), K - 1u);
    if (!tcSlotAccepts(lk.slots[subSlot], lk.normalTags[subSlot], surfNor_WS))
    {
        return 0u;
    }

    const uint leafIdx = rtslLightToLeaf[lk.slots[subSlot]];
    return lcAncestorAt(leafIdx, lcEffectiveLevels());
}

// =============================================
// MIS pdf recovery — mixture of root descent + cached subtree descents
// =============================================

// Probability the forward HIS descent reaches `areaLightIdx` when started from
// `subtreeRoot`, walking the LOW `levels` path bits of the light's leaf offset.
// Caller guarantees the light lives under subtreeRoot (shared L-prefix); if it
// did not, this would return a path pdf for the wrong leaf, so the prefix check
// in lcEvaluateMixturePdf is mandatory.
float lcEvaluateSubtreePdf(uint subtreeRoot, uint areaLightIdx,
                           float3 surfPos_WS, float3 surfNor_WS)
{
    const uint levels = lcEffectiveLevels();
    if (levels == 0u)
    {
        return 1.0f;
    }

    const uint leafIdx = rtslLightToLeaf[areaLightIdx];
    const uint pathOffset = leafIdx - rtslParams.treeLeafBase;

    uint cur = subtreeRoot;
    float pdf = 1.0f;

    [loop]
    for (uint i = 0u; i < levels; ++i)
    {
        const uint leftIdx = 2u * cur + 1u;
        const uint rightIdx = 2u * cur + 2u;
        const LightTreeNode leftNode = rtslLightTree[leftIdx];
        const LightTreeNode rightNode = rtslLightTree[rightIdx];

        float p1, p2;
        rtslChildProbs(leftNode, rightNode, surfPos_WS, surfNor_WS, p1, p2);
        if (p1 + p2 <= 0.0f)
        {
            return 0.0f;
        }

        // MSB-first within the L-level subtree: bit (levels-1) is the first
        // decision below subtreeRoot, mirroring evaluateLightSelectPdf's order.
        const uint bit = (pathOffset >> (levels - 1u - i)) & 1u;
        if (bit == 0u)
        {
            pdf *= p1;
            cur = leftIdx;
        }
        else
        {
            pdf *= p2;
            cur = rightIdx;
        }
    }

    return pdf;
}

// Mixture pdf for `areaLightIdx`, matching lcSelectSubtreeRoot's sampling law:
//   p_total = uniformFrac' * p_root
//           + (1 - uniformFrac) * (1/K) * Σ_{accepted s} p_subtree(root_s -> j)
// with uniformFrac' = uniformFrac + (1 - uniformFrac) * (K - numAccepted) / K.
// The (K - numAccepted) mass folds empty + dropped-light + normal-rejected
// slots into the root term exactly — each of those collapses to root on the
// sampler side. (Naive per-slot subtree walks here; step 4 hoists duplicates.)
float lcEvaluateMixturePdf(uint areaLightIdx,
                           uint2 currPixel,
                           float3 surfPos_WS, float3 surfNor_WS,
                           float currLinearDepth,
                           ByteAddressBuffer cachePrev,
                           Texture2D<float> linearDepthPrevTex,
                           Texture2D<float2> motionTex)
{
    const float pRoot = evaluateLightSelectPdf(areaLightIdx, surfPos_WS, surfNor_WS);

    if (rtslCacheParams.enabled == 0u || rtslParams.treeLeafCount == 0u)
    {
        return pRoot;
    }

    const TileCacheLookup lk = tcLookupReprojected(
        currPixel, surfNor_WS, currLinearDepth, cachePrev, linearDepthPrevTex, motionTex);
    if (!lk.valid)
    {
        return pRoot; // numAccepted == 0 → uniformFrac' == 1 → p_total == pRoot
    }

    const uint queryLeaf = rtslLightToLeaf[areaLightIdx];
    const uint levels = lcEffectiveLevels();
    const uint K = rtslCacheParams.lightsPerCell;
    // Same-subtree test: a cached slot contributes to this light only if its
    // leaf shares the high (logM - L) prefix, i.e. sits under the same L-ancestor.
    const uint queryPrefix = (queryLeaf == LEAF_IDX_INVALID)
        ? 0xFFFFFFFFu
        : ((queryLeaf - rtslParams.treeLeafBase) >> levels);
    const uint querySubtreeRoot = (queryLeaf == LEAF_IDX_INVALID)
        ? 0u
        : lcAncestorAt(queryLeaf, levels);

    float pSubtreeSum = 0.0f;
    uint numAccepted = 0u;

    [loop]
    for (uint s = 0u; s < K; ++s)
    {
        if (!tcSlotAccepts(lk.slots[s], lk.normalTags[s], surfNor_WS))
        {
            continue;
        }
        ++numAccepted;

        if (queryLeaf == LEAF_IDX_INVALID)
        {
            continue;
        }
        const uint cachedLeaf = rtslLightToLeaf[lk.slots[s]];
        if (((cachedLeaf - rtslParams.treeLeafBase) >> levels) != queryPrefix)
        {
            continue; // cached light is in a different subtree → contributes 0
        }
        pSubtreeSum += lcEvaluateSubtreePdf(querySubtreeRoot, areaLightIdx, surfPos_WS, surfNor_WS);
    }

    const float uniformFrac = rtslCacheParams.uniformFrac;
    const float uniformFracPrime = uniformFrac
        + (1.0f - uniformFrac) * float(K - numAccepted) / float(K);
    return uniformFracPrime * pRoot + (1.0f - uniformFrac) * pSubtreeSum / float(K);
}
