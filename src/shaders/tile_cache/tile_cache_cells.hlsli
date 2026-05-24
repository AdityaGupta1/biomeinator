// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// Light-tree-INDEPENDENT half of the screen-space tile cache: cell addressing,
// sub-bucket key, per-slot counter pack/rate, slot load, and motion-vector
// reprojection. Split out of tile_cache.hlsli so the carry pass
// (rtsl_tile_cache_carry.cs.hlsl) can reuse it WITHOUT pulling in the
// light-tree sampler / MIS pdf, which reference rtslLightTree /
// evaluateLightSelectPdf / rtslChildProbs that the carry pass never binds.
//
// Everything here touches only the GlobalParams cbuffer (renderParams /
// cameraParams / rtslCacheParams) and buffers passed in as parameters — no
// light-tree SRV. The light-tree-dependent half (tcSlotAccepts, the lc*
// sampler/pdf) lives in tile_cache.hlsli, which includes this header.

#include "../rendering/common/common_settings.h"
#include "common/global_params.hlsli"
#include "util/packing.hlsli"

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

// The slot tag word holds octEncode(normal_WS) (full 32-bit oct, not the
// 16-bit mask the plan sketched: masking drops the y component and the decode
// is unrecoverable; the tag word is a full u32 so we use all of it).
bool tcNormalTagAccepts(uint normalTagStored, float3 surfNor_WS)
{
    const float3 storedNor = octDecode(normalTagStored);
    return dot(storedNor, surfNor_WS) >= rtslCacheParams.rejectNormalCos;
}

// =============================================
// Cell addressing
// =============================================

uint tcTilesX()
{
    return (renderParams.renderSize.x + RTSL_TILE_PIXELS - 1u) / RTSL_TILE_PIXELS;
}

// Returns the SLOT index of slot 0 of a cell; callers scale by
// RTSL_TILE_CACHE_SLOT_BYTES for the byte offset.
uint tcSlotBase(uint2 tile, uint subBucket)
{
    const uint tileLinear = tile.y * tcTilesX() + tile.x;
    return (tileLinear * RTSL_TILE_SUB_BUCKETS + subBucket) * RTSL_LIGHT_CACHE_K_MAX;
}

// =============================================
// Per-light visibility counters
// =============================================

// A 16-byte slot is four u32 words:
//   +0 lightIdx   +4 normalTag   +8 attempts   +12 successes
// attempts/successes are fixed-point (scale RTSL_CACHE_STAT_SCALE). The carry
// pass decays them; weighted eviction (W4) reads them.

// Round (not floor) and clamp a fractional count back to a stored counter word.
// Rounding keeps the carry-pass decay matching the nominal rate instead of
// biasing steeper at low counts; the clamp keeps the EWMA fixed point in range.
uint tcPackCounter(float value)
{
    return (uint)clamp(value + 0.5f, 0.0f, (float)RTSL_CACHE_COUNTER_MAX);
}

// Success rate from fixed-point counters; the scale cancels, and max() guards a
// fresh (0, 0) slot against divide-by-zero.
float tcCounterRate(uint attempts, uint successes)
{
    return (float)successes / (float)max(attempts, 1u);
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

// Loads the active slots of one (tile, sub-bucket) cell from a read-only Prev
// buffer. Reads K_MAX entries (compile-time loop bound); slots beyond
// lightsPerCell stay INVALID from the frame's carry and are never accepted.
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
// Reprojection — relative-depth disocclusion
// =============================================

// Shared read-path/carry disocclusion test: rejects past-an-edge reprojections
// AND the "prev pixel was sky" case (prev depth ~ farPlane -> large delta).
bool tcDepthRejects(float currLinearDepth, float prevLinearDepth)
{
    const float depthRel = abs(currLinearDepth - prevLinearDepth) /
                           max(currLinearDepth, prevLinearDepth);
    return depthRel > rtslCacheParams.rejectDepthRel;
}

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
    if (tcDepthRejects(currLinearDepth, prevLinearDepth))
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
