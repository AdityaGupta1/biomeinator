// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "light/ris.hlsli"
#include "util/packing.hlsli"
#include "util/ray.hlsli"
#include "util/rng.hlsli"

#define NUM_REPROJECTION_ATTEMPTS 9
#define REPROJECTION_SEARCH_RADIUS 4
#define REPROJECTION_MAX_POSITION_DIST 0.2f // TODO: set this based on depth? (i.e. higher max dist at higher depth)
#define REPROJECTION_MIN_NORMAL_DOT 0.95f

StructuredBuffer<RisSample> risSamplesIn : REGISTER_T(TEMPORAL_REUSE, RIS_SAMPLES_IN);
StructuredBuffer<RisSample> risSamplesPrev : REGISTER_T(TEMPORAL_REUSE, RIS_SAMPLES_PREV);

RWStructuredBuffer<RisSample> risSamplesOut : REGISTER_U(TEMPORAL_REUSE, RIS_SAMPLES_OUT);

struct ReprojectionResult
{
    bool found;
    uint2 pixelIdx;

    float3 this_surfPos_WS;
    float3 this_surfNor_WS;

    float3 reproj_surfPos_WS;
    float3 reproj_surfNor_WS;
};

// Deterministic per-frame 4x4 cross-shuffle of the temporal fetch position (from RTXDI).
// Decorrelates neighboring pixels' temporal reuse chains so coherent sample blobs can't
// slowly crawl across the screen.
void applyPermutationSampling(inout int2 pixelIdx, const uint uniformRandomNumber)
{
    const int2 offset = int2(uniformRandomNumber & 3, (uniformRandomNumber >> 2) & 3);
    pixelIdx += offset;
    pixelIdx.x ^= 3;
    pixelIdx.y ^= 3;
    pixelIdx -= offset;
}

ReprojectionResult reproject(const uint2 pixelIdx, inout RandomNumberGenerator rng)
{
    ReprojectionResult result;
    result.found = false;

    Texture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.srv.motionTargetIdx];
    const float2 motionPixels = motionTarget[pixelIdx] * renderParams.renderSize;
    float2 reprojectedPixelPos = float2(pixelIdx) + cameraParams.jitter + motionPixels; // fractional pixel pos where this world pos was last frame

    const bool usePermutationSampling = (renderParams.restirEnablePermutationSampling == 1);
    if (!usePermutationSampling)
    {
        // stochastic bilinear: a random +-0.5px jitter before rounding makes the expected
        // reprojection position unbiased, avoiding directional drift of reused samples
        reprojectedPixelPos += rng.nextFloat2() - 0.5f;
    }

    Texture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float3 this_surfPos_WS = cameraParams.pos_WS + getPrimaryRayDirection(pixelIdx) * linearDepthTarget[pixelIdx];

    Texture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.srv.normalsAndRoughnessTargetIdx];
    const float3 this_surfNor_WS = normalsAndRoughnessTarget[pixelIdx].xyz;

    Texture2D<uint2> prevDepthAndNormalTarget = ResourceDescriptorHeap[heapIndices.srv.prevDepthAndNormalTargetIdx];

    const int2 basePixelIdx = int2(round(reprojectedPixelPos));
    for (uint attemptIdx = 0; attemptIdx < NUM_REPROJECTION_ATTEMPTS; ++attemptIdx)
    {
        int2 reprojCandidatePixelIdx = basePixelIdx;
        if (attemptIdx > 0)
        {
            // disocclusion fallback: search a small random neighborhood
            reprojCandidatePixelIdx += int2((rng.nextFloat2() - 0.5f) * REPROJECTION_SEARCH_RADIUS);
        }
        else if (usePermutationSampling)
        {
            applyPermutationSampling(reprojCandidatePixelIdx, constantParams.rngSeed);
        }

        if (isPixelOutOfBounds(reprojCandidatePixelIdx))
        {
            continue;
        }

        const uint2 packedReprojDepthAndNormal = prevDepthAndNormalTarget[reprojCandidatePixelIdx];
        const float reproj_depth = asfloat(packedReprojDepthAndNormal.x);
        const float3 reproj_surfNor_WS = octDecode(packedReprojDepthAndNormal.y);
        const float3 reproj_surfPos_WS = cameraParams.prevPos_WS + getPrevPrimaryRayDirection(reprojCandidatePixelIdx) * reproj_depth;

        // accept the first candidate whose surface matches; a deterministic "best match"
        // search would re-introduce directional reprojection bias
        if (distance(this_surfPos_WS, reproj_surfPos_WS) > REPROJECTION_MAX_POSITION_DIST ||
            dot(this_surfNor_WS, reproj_surfNor_WS) < REPROJECTION_MIN_NORMAL_DOT)
        {
            continue;
        }

        result.found = true;
        result.pixelIdx = uint2(reprojCandidatePixelIdx);

        result.this_surfPos_WS = this_surfPos_WS;
        result.this_surfNor_WS = this_surfNor_WS;

        result.reproj_surfPos_WS = reproj_surfPos_WS;
        result.reproj_surfNor_WS = reproj_surfNor_WS;
        break;
    }

    return result;
}

[shader("compute")]
[numthreads(TEMPORAL_REUSE_WORKGROUP_SIZE_X, TEMPORAL_REUSE_WORKGROUP_SIZE_Y, 1)]
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

    RandomNumberGenerator rng = initRng(constantParams.rngSeed, 44721359, linearPixelIdx, renderParams.frameNumber);

    const ReprojectionResult reprojResult = reproject(pixelIdx, rng);

    if (!reprojResult.found)
    {
        risSamplesOut[linearPixelIdx] = this_risSample;
        return;
    }

    const uint reproj_linearPixelIdx = reprojResult.pixelIdx.y * renderParams.renderSize.x + reprojResult.pixelIdx.x;
    const RisSample reproj_risSample = risSamplesPrev[reproj_linearPixelIdx];

    if (reproj_risSample.lightIdx == LIGHT_IDX_INVALID)
    {
        risSamplesOut[linearPixelIdx] = this_risSample;
        return;
    }

    //if (!isfinite(reproj_risSample.W)) // re-enable this if NaNs start spreading across the screen again (see #193)
    //{
    //    risSamplesOut[linearPixelIdx] = this_risSample;
    //    return;
    //}

    const float this_confidence = this_risSample.confidence;
    const float reproj_confidence = reproj_risSample.confidence;

    const AreaLight this_light = areaLights[this_risSample.lightIdx];
    const AreaLight reproj_light = areaLights[reproj_risSample.lightIdx];

    const float this_p_hat = this_risSample.p_hat;
    const float this_p_hat_reproj = risTargetFunction(this_light, this_risSample.pointOnLight_WS, reprojResult.reproj_surfPos_WS, reprojResult.reproj_surfNor_WS); // this_p_hat from reproj_pos
    const float this_m_numerator = this_p_hat * this_confidence;
    const float this_m_denominator = this_m_numerator + (this_p_hat_reproj * reproj_confidence);
    const float this_m = (this_m_denominator > 0.f) ? (this_m_numerator / this_m_denominator) : 0.f;

    const float reproj_p_hat = reproj_risSample.p_hat;
    const float reproj_p_hat_this = risTargetFunction(reproj_light, reproj_risSample.pointOnLight_WS, reprojResult.this_surfPos_WS, reprojResult.this_surfNor_WS); // reproj_p_hat from this_pos
    const float reproj_m_numerator = reproj_p_hat * reproj_confidence;
    const float reproj_m_denominator = reproj_m_numerator + (reproj_p_hat_this * this_confidence);
    const float reproj_m = (reproj_m_denominator > 0.f) ? (reproj_m_numerator / reproj_m_denominator) : 0.f;

    const float this_w = this_m * this_p_hat * this_risSample.W;
    const float reproj_w = reproj_m * reproj_p_hat_this * reproj_risSample.W;
    const float w_sum = this_w + reproj_w;

    if (!(w_sum > 0.f)) // also catches NaN
    {
        risSamplesOut[linearPixelIdx] = this_risSample;
        return;
    }

    RisSample risSampleOut;
    float Y_p_hat;
    if (rng.nextFloat() < this_w / w_sum)
    {
        risSampleOut.lightIdx = this_risSample.lightIdx;
        risSampleOut.pointOnLight_WS = this_risSample.pointOnLight_WS;
        Y_p_hat = this_p_hat;
    }
    else
    {
        risSampleOut.lightIdx = reproj_risSample.lightIdx;
        risSampleOut.pointOnLight_WS = reproj_risSample.pointOnLight_WS;
        Y_p_hat = reproj_p_hat_this;
    }
    risSampleOut.W = sanitizeFloat(w_sum / Y_p_hat, 0.f);
    risSampleOut.p_hat = Y_p_hat;
    risSampleOut.confidence = min(uint(this_confidence + reproj_confidence), RESTIR_MAX_CONFIDENCE);
    risSampleOut.pad0 = 0;
    risSamplesOut[linearPixelIdx] = risSampleOut;
}
