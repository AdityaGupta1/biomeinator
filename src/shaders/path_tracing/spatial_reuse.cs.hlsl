// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "light/ris.hlsli"
#include "util/rng.hlsli"

#define NUM_SPATIAL_SAMPLES 5
#define SPATIAL_SAMPLE_MAX_RADIUS 32

StructuredBuffer<RisSample> risSamplesIn : REGISTER_T(SPATIAL_REUSE, RIS_SAMPLES_IN);

RWStructuredBuffer<RisSample> risSamplesOut : REGISTER_U(SPATIAL_REUSE, RIS_SAMPLES_OUT);

// Confidence-weighted pairwise MIS (from RTXDI): each neighbor is MIS'd against the
// canonical (center) sample only, which is O(N) and folds confidence weights in without
// needing a preliminary pass to sum them across all neighbors.
float pairwiseMisWeight(const float p_atOwn, const float p_atOther, const float ownM, const float otherM)
{
    const float balanceDenominator = ownM * p_atOwn + otherM * p_atOther;
    return (balanceDenominator > 0.f) ? max(0.f, ownM * p_atOwn) / balanceDenominator : 0.f;
}

// Discounts a neighbor's confidence contribution when its target function disagrees with
// the canonical surface's, so dissimilar neighbors can't inflate the merged history.
float pairwiseMFactor(const float p_atOwn, const float p_atOther)
{
    return (p_atOwn <= 0.f) ? 1.f : pow(saturate(p_atOther / p_atOwn), 8.f);
}

[shader("compute")]
[numthreads(SPATIAL_REUSE_WORKGROUP_SIZE_X, SPATIAL_REUSE_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixelIdx = dispatchThreadId.xy;

    if (any(pixelIdx >= renderParams.renderSize))
    {
        return;
    }

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const RisSample this_risSample = risSamplesIn[linearPixelIdx];
    if (this_risSample.lightIdx == LIGHT_IDX_INVALID)
    {
        risSamplesOut[linearPixelIdx] = this_risSample;
        return;
    }

    RandomNumberGenerator rng = initRng(constantParams.rngSeed, 1908061, linearPixelIdx, renderParams.frameNumber);

    Texture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float3 this_surfPos_WS = cameraParams.pos_WS + getPrimaryRayDirection(pixelIdx) * linearDepthTarget[pixelIdx];

    Texture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.srv.normalsAndRoughnessTargetIdx];
    const float3 this_surfNor_WS = normalsAndRoughnessTarget[pixelIdx].xyz;

    uint Y_lightIdx = LIGHT_IDX_INVALID;
    float Y_p_hat = 0.f;
    float3 Y_pointOnLight_WS = 0.f;

    const AreaLight this_light = areaLights[this_risSample.lightIdx];
    const float this_p_hat = this_risSample.p_hat;
    const float this_confidence = this_risSample.confidence;

    float w_sum = 0.f;
    float canonicalMisWeight = 0.f; // accumulates this pixel's share of each pairwise MIS pair
    float sumConfidence = this_confidence;
    uint numValidSpatialSamples = 0;
    for (uint spatialSampleIdx = 0; spatialSampleIdx < NUM_SPATIAL_SAMPLES; ++spatialSampleIdx)
    {
        const float2 spatialSamplePixelOffset = float2((rng.nextFloat2() - 0.5f) * 2 * SPATIAL_SAMPLE_MAX_RADIUS);
        const int2 spatialSamplePixelIdx = int2(pixelIdx) + int2(round(spatialSamplePixelOffset));
        if (isPixelOutOfBounds(spatialSamplePixelIdx))
        {
            continue;
        }

        const float3 other_surfPos_WS = cameraParams.pos_WS + getPrimaryRayDirection(spatialSamplePixelIdx) * linearDepthTarget[spatialSamplePixelIdx];
        const float3 other_surfNor_WS = normalsAndRoughnessTarget[spatialSamplePixelIdx].xyz;
        if (dot(this_surfNor_WS, other_surfNor_WS) < 0.95f || distance(this_surfPos_WS, other_surfPos_WS) > 0.4f)
        {
            continue;
        }

        const uint spatialSampleLinearPixelIdx = spatialSamplePixelIdx.y * renderParams.renderSize.x + spatialSamplePixelIdx.x;
        const RisSample other_risSample = risSamplesIn[spatialSampleLinearPixelIdx];

        // similar surface = usable neighbor; counted even when its reservoir is dead so the
        // final normalization stays unbiased
        ++numValidSpatialSamples;

        if (other_risSample.lightIdx == LIGHT_IDX_INVALID || other_risSample.confidence == 0)
        {
            continue;
        }

        const AreaLight other_light = areaLights[other_risSample.lightIdx];
        const float other_confidence = other_risSample.confidence;

        // target function of each sample at each surface; stored p_hat is always evaluated
        // at the storing pixel's current-frame surface, so it can be reused directly
        const float other_p_hat = other_risSample.p_hat;
        const float other_p_hat_this = risTargetFunction(other_light, other_risSample.pointOnLight_WS, this_surfPos_WS, this_surfNor_WS); // other_p_hat from this_pos
        const float this_p_hat_other = risTargetFunction(this_light, this_risSample.pointOnLight_WS, other_surfPos_WS, other_surfNor_WS); // this_p_hat from other_pos

        const float otherStreamM = other_confidence * NUM_SPATIAL_SAMPLES;
        const float other_m = pairwiseMisWeight(other_p_hat, other_p_hat_this, otherStreamM, this_confidence);
        const float canonical_m = pairwiseMisWeight(this_p_hat_other, this_p_hat, otherStreamM, this_confidence);
        canonicalMisWeight += 1.f - canonical_m;

        const float other_w = other_m * other_p_hat_this * other_risSample.W;

        w_sum += other_w;
        if (rng.nextFloat() < other_w / w_sum)
        {
            Y_lightIdx = other_risSample.lightIdx;
            Y_p_hat = other_p_hat_this;
            Y_pointOnLight_WS = other_risSample.pointOnLight_WS;
        }

        sumConfidence += other_confidence * min(pairwiseMFactor(other_p_hat, other_p_hat_this),
                                                pairwiseMFactor(this_p_hat_other, this_p_hat));
    }

    if (numValidSpatialSamples == 0)
    {
        canonicalMisWeight = 1.f;
    }

    const float this_w = canonicalMisWeight * this_p_hat * this_risSample.W;
    w_sum += this_w;
    if (rng.nextFloat() < this_w / w_sum)
    {
        Y_lightIdx = this_risSample.lightIdx;
        Y_p_hat = this_p_hat;
        Y_pointOnLight_WS = this_risSample.pointOnLight_WS;
    }

    RisSample risSampleOut;
    risSampleOut.lightIdx = Y_lightIdx;
    risSampleOut.pointOnLight_WS = Y_pointOnLight_WS;
    risSampleOut.W = sanitizeFloat(w_sum / (Y_p_hat * max(numValidSpatialSamples, 1)), 0.f);
    risSampleOut.p_hat = Y_p_hat;
    risSampleOut.confidence = min(uint(round(sumConfidence)), RESTIR_MAX_CONFIDENCE);
    risSampleOut.pad0 = 0;
    risSamplesOut[linearPixelIdx] = risSampleOut;
}
