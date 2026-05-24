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
// Upsert path (write this frame's Curr buffer + accumulate outcome counters)
// =============================================

// Find-or-insert `lightIdx` in its (tile, sub-bucket) cell, accumulating the
// outcome (dAttempts, dSuccesses) onto its visibility counters. All counter
// values are fixed-point (scale RTSL_CACHE_STAT_SCALE). On a fresh insert the
// slot is seeded from the carried (decayed Prev) counters (seedAttempts,
// seedSuccesses) so X keeps its history instead of restarting at the prior.
//
// Concurrency: empty/full slots are claimed via InterlockedCompareExchange on
// the lightIdx word, never a plain Store, so persistent counters can't tear
// against concurrent InterlockedAdds from other lanes crediting the same slot
// (weighted_plan.md audit fix #4). The freshly-claimed slot's counters are
// guaranteed (0, 0) by the carry pass, so seeding via InterlockedAdd composes
// correctly with any same-light dup-adds that race in.
//
// W4: Pass 2 (cell full) evicts the slot with the lowest confidence-shrunk
// success rate (weighted eviction) rather than a uniform-random victim, so a
// consistently occluded light is dropped before a visible one. Selection is
// deterministic, so no RNG is drawn here anymore.
void tcUpsert(RWByteAddressBuffer cacheCurr,
              uint2 currPixel,
              float linearDepth,
              float3 normal_WS,
              uint lightIdx,
              uint dAttempts,
              uint dSuccesses,
              uint seedAttempts,
              uint seedSuccesses)
{
    // Storing the empty sentinel would permanently block this slot's dup/empty
    // scan; callers pass a resolved light index, but bail defensively.
    if (lightIdx == LIGHT_IDX_INVALID)
    {
        return;
    }

    const uint2 tile = currPixel / RTSL_TILE_PIXELS;
    const uint subBucket = tcSubBucket(linearDepth, normal_WS);
    const uint slotBase = tcSlotBase(tile, subBucket);
    const uint normalTag = octEncode(normal_WS);
    const uint K = rtslCacheParams.lightsPerCell;

    // Pass 1a: read-only scan for an existing slot for this light. Finding a
    // resident copy BEFORE claiming an empty slot is what stops one light's
    // counters being split across two slots when an empty slot precedes the
    // resident one (e.g. a low slot freed by the carry's dead-light drop) — the
    // single-CAS-pass form would claim the empty slot first and duplicate the
    // light. A plain Load of the lightIdx word is atomic; a concurrent claim only
    // flips it to its old or new value.
    for (uint s = 0u; s < K; ++s)
    {
        const uint off = (slotBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
        if (cacheCurr.Load(off) == lightIdx)
        {
            // Already resident — accumulate just this sample's outcome.
            cacheCurr.Store(off + 4u, normalTag);
            if (dAttempts != 0u)
            {
                cacheCurr.InterlockedAdd(off + 8u, dAttempts);
            }
            if (dSuccesses != 0u)
            {
                cacheCurr.InterlockedAdd(off + 12u, dSuccesses);
            }
            return;
        }
    }

    // Pass 1b: not resident — claim the first empty slot via CAS. A lane that
    // raced us to insert the same light between the scan above and here is caught
    // by the existing==lightIdx branch (add the delta, don't duplicate); only the
    // residual two-first-inserters race can still duplicate, which is bounded and
    // self-heals at the next carry (matches the plan's find-then-add note).
    for (uint s = 0u; s < K; ++s)
    {
        const uint off = (slotBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
        uint existing;
        cacheCurr.InterlockedCompareExchange(off, LIGHT_IDX_INVALID, lightIdx, existing);
        if (existing == LIGHT_IDX_INVALID)
        {
            // Won a fresh slot (carry left its counters at 0) — seed from X's
            // carried history plus this sample's outcome.
            cacheCurr.Store(off + 4u, normalTag);
            const uint a0 = seedAttempts + dAttempts;
            const uint s0 = seedSuccesses + dSuccesses;
            if (a0 != 0u)
            {
                cacheCurr.InterlockedAdd(off + 8u, a0);
            }
            if (s0 != 0u)
            {
                cacheCurr.InterlockedAdd(off + 12u, s0);
            }
            return;
        }
        if (existing == lightIdx)
        {
            // Raced — another lane just inserted our light here. Accumulate the
            // delta only (it already seeded from the carried history).
            cacheCurr.Store(off + 4u, normalTag);
            if (dAttempts != 0u)
            {
                cacheCurr.InterlockedAdd(off + 8u, dAttempts);
            }
            if (dSuccesses != 0u)
            {
                cacheCurr.InterlockedAdd(off + 12u, dSuccesses);
            }
            return;
        }
    }

    // Pass 2: cell full, no match. Weighted eviction (weighted_plan.md W4): the
    // victim is the slot with the lowest confidence-shrunk success rate
    //   rate_est = (successes + priorSuccesses) / (attempts + priorAttempts),
    // so a consistently occluded light (rate_est -> 0) is evicted first. The
    // prior is RELATIVE TO THE CELL (audit fix #7): priorMean is the cell's own
    // mean observed rate, so "fresh" means "typical for this cell" rather than an
    // absolute 0.5 — otherwise a well-lit cell (every resident -> 1) would thrash
    // each newcomer and a fully-occluded cell would let one squat. Ties break by
    // the lower attempts, evicting the less-tested slot so an earned mid rate
    // beats an untested newcomer at the same rate_est.
    //
    // The victim is CAS-claimed on its lightIdx word; a lost CAS means another
    // lane moved the cell under us, so we re-scan (bounded by K retries). Counter
    // adds that raced in before the claim are discarded by the overwrite —
    // bounded and self-healing, like Pass 1b.
    const float priorAttempts = float(RTSL_CACHE_STAT_SCALE) * rtslCacheParams.evictPriorStrength;
    [loop]
    for (uint t = 0u; t < K; ++t)
    {
        // Cell mean observed rate -> the per-slot prior mean. Only slots with
        // recorded attempts contribute (a 0-attempt membership-vote slot has no
        // observed rate and would otherwise drag the prior to 0). Falls back to
        // 0.5 when no slot has evidence yet.
        float rateSum = 0.0f;
        uint evidenced = 0u;
        [loop]
        for (uint s = 0u; s < K; ++s)
        {
            const uint off = (slotBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
            const uint a = cacheCurr.Load(off + 8u);
            if (a != 0u)
            {
                rateSum += tcCounterRate(a, cacheCurr.Load(off + 12u));
                ++evidenced;
            }
        }
        const float priorMean = (evidenced != 0u) ? (rateSum / float(evidenced)) : 0.5f;
        const float priorSuccesses = priorMean * priorAttempts;

        // Pick the victim = min rate_est, ties by lower attempts. A concurrent
        // insert of our own light is caught here too (accumulate and stop).
        uint victimSlot = 0u;
        uint victimLight = LIGHT_IDX_INVALID;
        float bestRate = 2.0f; // rate_est in [0, 1]; sentinel above the max
        uint bestAttempts = 0xFFFFFFFFu;
        [loop]
        for (uint s = 0u; s < K; ++s)
        {
            const uint off = (slotBase + s) * RTSL_TILE_CACHE_SLOT_BYTES;
            const uint light = cacheCurr.Load(off);
            if (light == lightIdx)
            {
                cacheCurr.Store(off + 4u, normalTag);
                if (dAttempts != 0u)
                {
                    cacheCurr.InterlockedAdd(off + 8u, dAttempts);
                }
                if (dSuccesses != 0u)
                {
                    cacheCurr.InterlockedAdd(off + 12u, dSuccesses);
                }
                return;
            }
            const uint a = cacheCurr.Load(off + 8u);
            const uint sc = cacheCurr.Load(off + 12u);
            // denom is 0 only when evictPriorStrength == 0 (no prior) AND the slot
            // is untested (a == 0): with no prior an untested slot has no evidence
            // of visibility, so rank it as the most-evictable (rate 0) rather than
            // 0/0 = NaN, which would compare false against everything and either
            // exclude it from eviction or, in an all-untested full cell, leave the
            // victim INVALID and silently drop the insert. With the default prior
            // (> 0) denom is always > 0 and this branch never fires.
            const float denom = float(a) + priorAttempts;
            const float rateEst = (denom > 0.0f) ? (float(sc) + priorSuccesses) / denom : 0.0f;
            if (rateEst < bestRate || (rateEst == bestRate && a < bestAttempts))
            {
                bestRate = rateEst;
                bestAttempts = a;
                victimSlot = s;
                victimLight = light;
            }
        }

        const uint victimOff = (slotBase + victimSlot) * RTSL_TILE_CACHE_SLOT_BYTES;
        uint prev;
        cacheCurr.InterlockedCompareExchange(victimOff, victimLight, lightIdx, prev);
        if (prev == victimLight)
        {
            cacheCurr.Store(victimOff + 4u, normalTag);
            cacheCurr.Store(victimOff + 8u, seedAttempts + dAttempts);
            cacheCurr.Store(victimOff + 12u, seedSuccesses + dSuccesses);
            return;
        }
    }
}

// =============================================
// Mixture sampler — pick a subtree root for selectLightFromSubtree
// =============================================

// Returns the subtree root the forward HIS descent should start from. 0u
// (tree root) means "no cache contribution this sample". The caller feeds the
// result straight into selectLightFromSubtree.
//
// `seed` reports the cached light X this sample descended from (and its decayed
// Prev counters) for outcome attribution; seed.valid is false on every path that
// returns 0u (uniform branch / miss / rejected slot — no cached seed used).
//
// Byte-identical-when-disabled: every early-out below precedes any nextFloat()
// call, so with the cache off the RNG sequence matches the pre-cache path. The
// seed thread-through draws no extra random.
uint lcSelectSubtreeRoot(uint2 currPixel,
                         float3 surfNor_WS,
                         float currLinearDepth,
                         ByteAddressBuffer cachePrev,
                         Texture2D<float> linearDepthPrevTex,
                         Texture2D<float2> motionTex,
                         out TileCacheSeed seed,
                         inout RandomNumberGenerator rng)
{
    seed = tcSeedNull();

    if (rtslCacheParams.enabled == 0u || rtslParams.treeLeafCount == 0u)
    {
        return 0u;
    }

    const float coin = rng.nextFloat();
    if (coin < rtslCacheParams.uniformFrac)
    {
        return 0u;
    }

    uint prevSlotBase;
    const TileCacheLookup lk = tcLookupReprojected(
        currPixel, surfNor_WS, currLinearDepth, cachePrev, linearDepthPrevTex, motionTex, prevSlotBase);
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

    // X = the cached light this descent seeds from. Read its decayed counters
    // straight from the Prev slot the reprojection already resolved (two loads
    // on the accept path — no second reprojection).
    const uint seedOff = (prevSlotBase + subSlot) * RTSL_TILE_CACHE_SLOT_BYTES;
    seed.valid = true;
    seed.lightIdx = lk.slots[subSlot];
    seed.attempts = cachePrev.Load(seedOff + 8u);
    seed.successes = cachePrev.Load(seedOff + 12u);

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

    uint pdfPrevSlotBase; // the pdf has no use for the counters, only the slots
    const TileCacheLookup lk = tcLookupReprojected(
        currPixel, surfNor_WS, currLinearDepth, cachePrev, linearDepthPrevTex, motionTex, pdfPrevSlotBase);
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
