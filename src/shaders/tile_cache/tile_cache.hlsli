// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// RTSL screen-space tile light cache: insert, the mixture sampler, and the MIS
// pdf that splice a cached-subtree descent into the per-pixel root descent of
// light_tree_sampling.hlsli. The light-tree-INDEPENDENT half (cell addressing,
// reprojection, counter helpers) lives in tile_cache_cells.hlsli, included
// below and shared with the carry pass.
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

#include "tile_cache/tile_cache_cells.hlsli"

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
// Per-slot accept predicate — single source of truth (sampler == pdf)
// =============================================

// "Is slot s usable as a cached subtree seed?" Both the sampler and the pdf
// MUST call this with identical arguments so their accept sets never diverge —
// any divergence biases the MIS estimator. The carry pass applies the same
// lightIdx / rtslLightToLeaf dead-light test inline (it has no normal to gate).
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
// sampler side.
//
// Pdf hoist (plan step 4): a slot contributes a nonzero p_subtree to `j` only
// when its cached light shares j's high (logM - L) prefix — and every such slot
// then descends from the SAME root (j's L-ancestor, see lcAncestorAt prefix
// algebra), so each contributes the identical lcEvaluateSubtreePdf value. The
// per-slot sum therefore collapses to (count of matching slots) × one subtree
// walk, replacing the naive K walks.
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

    uint numAccepted = 0u;
    uint numMatching = 0u; // accepted AND sharing the query's L-prefix

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
        if (((cachedLeaf - rtslParams.treeLeafBase) >> levels) == queryPrefix)
        {
            ++numMatching; // shares the subtree → contributes one identical walk
        }
    }

    // One subtree walk, scaled by the matching-slot count (the hoist).
    const float pSubtreeSum = (numMatching > 0u)
        ? float(numMatching) * lcEvaluateSubtreePdf(querySubtreeRoot, areaLightIdx, surfPos_WS, surfNor_WS)
        : 0.0f;

    const float uniformFrac = rtslCacheParams.uniformFrac;
    const float uniformFracPrime = uniformFrac
        + (1.0f - uniformFrac) * float(K - numAccepted) / float(K);
    return uniformFracPrime * pRoot + (1.0f - uniformFrac) * pSubtreeSum / float(K);
}
