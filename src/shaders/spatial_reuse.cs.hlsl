/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
#include "path_tracing_common.hlsli"
#include "restir.hlsli"
#include "util/rng.hlsli"

#define NUM_SPATIAL_SAMPLES 4
#define SPATIAL_SAMPLE_MAX_RADIUS 10

StructuredBuffer<RisSample> risSamplesIn : REGISTER_T(SPATIAL_REUSE_REGISTER_RIS_SAMPLES_IN, SPATIAL_REUSE_REGISTER_SPACE);

RWStructuredBuffer<RisSample> risSamplesOut : REGISTER_U(SPATIAL_REUSE_REGISTER_RIS_SAMPLES_OUT, SPATIAL_REUSE_REGISTER_SPACE);

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

    const RisSample thisRisSample = risSamplesIn[linearPixelIdx];
    if (thisRisSample.lightIdx == LIGHT_IDX_INVALID)
    {
        risSamplesOut[linearPixelIdx] = thisRisSample;
        return;
    }

    RandomSampler rng = initRandomSampler(constantParams.rngSeed ^ 1908061, linearPixelIdx, renderParams.frameNumber);

    Texture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float3 primaryRayDirection = getPrimaryRayDirection(pixelIdx);
    const float3 this_surfPos_WS = cameraParams.pos_WS + primaryRayDirection * linearDepthTarget[pixelIdx];

    Texture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.srv.normalsAndRoughnessTargetIdx];
    const float3 this_surfNor_WS = normalsAndRoughnessTarget[pixelIdx].xyz;

    uint Y_lightIdx = thisRisSample.lightIdx;
    float3 Y_pointOnLight_WS = thisRisSample.pointOnLight_WS;
    float Y_p_hat = risTargetFunction(areaLights[Y_lightIdx], this_surfPos_WS, this_surfNor_WS, Y_pointOnLight_WS);

    const float this_m = 1.f / (NUM_SPATIAL_SAMPLES + 1); // TODO: use better MIS weights (pairwise?)
    float w_sum = this_m * Y_p_hat * thisRisSample.W; // = this_w

    uint numValidSpatialSamples = 0;
    for (uint spatialSampleIdx = 0; spatialSampleIdx < NUM_SPATIAL_SAMPLES; ++spatialSampleIdx)
    {
        const float2 spatialSamplePixelOffset = float2((rng.nextFloat2() - 0.5f) * 2 * SPATIAL_SAMPLE_MAX_RADIUS);
        const uint2 spatialSamplePixelIdx = uint2(pixelIdx + int2(round(spatialSamplePixelOffset)));
        if (any(spatialSamplePixelIdx >= renderParams.renderSize)) // this might be kind of sus since it relies on underflow?
        {
            continue;
        }

        const float3 other_surfNor_WS = normalsAndRoughnessTarget[spatialSamplePixelIdx].xyz;
        if (dot(this_surfNor_WS, other_surfNor_WS) < 0.9f) // TODO: use same check as temporal reuse (compare pos_WS and normal) and turn it into a function in restir.hlsli that takes in this pos/nor and other pos/nor and returns similarity score
        {
            continue;
        }

        const uint spatialSampleLinearPixelIdx = spatialSamplePixelIdx.x + renderParams.renderSize.x * spatialSamplePixelIdx.y;
        const RisSample spatialRisSample = risSamplesIn[spatialSampleLinearPixelIdx];
        if (spatialRisSample.lightIdx == LIGHT_IDX_INVALID)
        {
            continue;
        }

        const AreaLight other_light = areaLights[spatialRisSample.lightIdx];

        const float3 this_wi_WS = normalize(spatialRisSample.pointOnLight_WS - this_surfPos_WS);
        const float this_r2 = distance2(this_surfPos_WS, spatialRisSample.pointOnLight_WS);
        const float this_geomTerm = absCosTheta(-this_wi_WS, other_light.normal_WS) / this_r2;

        const float3 other_surfPos_WS = cameraParams.pos_WS + getPrimaryRayDirection(spatialSamplePixelIdx) * linearDepthTarget[spatialSamplePixelIdx];
        const float3 other_wi_WS = normalize(spatialRisSample.pointOnLight_WS - other_surfPos_WS);
        const float other_r2 = distance2(other_surfPos_WS, spatialRisSample.pointOnLight_WS);
        const float other_geomTerm = absCosTheta(-other_wi_WS, other_light.normal_WS) / other_r2;

        const float geomTermJacobian = (this_geomTerm / max(other_geomTerm, 0.01f)); // TODO: better way to clamp fireflies?

        const float W = spatialRisSample.W * geomTermJacobian;

        const float m = 1.f / (NUM_SPATIAL_SAMPLES + 1); // TODO: use better MIS weights (pairwise?)
        const float p_hat = risTargetFunction(other_light, this_surfPos_WS, this_surfNor_WS, spatialRisSample.pointOnLight_WS);
        const float w = m * p_hat * W;

        w_sum += w;
        if (rng.nextFloat() < w / w_sum)
        {
            Y_lightIdx = spatialRisSample.lightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = spatialRisSample.pointOnLight_WS;
        }
        ++numValidSpatialSamples;
    }

    const float validSpatialSamplesCorrectionFactor = ((NUM_SPATIAL_SAMPLES + 1) / float(numValidSpatialSamples + 1));

    RisSample risSampleOut;
    risSampleOut.lightIdx = Y_lightIdx;
    risSampleOut.pointOnLight_WS = Y_pointOnLight_WS;
    risSampleOut.W = (w_sum / Y_p_hat) * validSpatialSamplesCorrectionFactor;
    risSampleOut.pad0 = risSampleOut.pad1 = risSampleOut.pad2 = 0;
    risSamplesOut[linearPixelIdx] = risSampleOut;
}
