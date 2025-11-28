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

    const RisSample this_risSample = risSamplesIn[linearPixelIdx];
    if (this_risSample.lightIdx == LIGHT_IDX_INVALID)
    {
        risSamplesOut[linearPixelIdx] = this_risSample;
        return;
    }

    RandomSampler rng = initRandomSampler(constantParams.rngSeed, 1908061, linearPixelIdx, renderParams.frameNumber);

    Texture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float3 this_surfPos_WS = cameraParams.pos_WS + getPrimaryRayDirection(pixelIdx) * linearDepthTarget[pixelIdx];

    Texture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.srv.normalsAndRoughnessTargetIdx];
    const float3 this_surfNor_WS = normalsAndRoughnessTarget[pixelIdx].xyz;

    uint Y_lightIdx = this_risSample.lightIdx;
    float3 Y_pointOnLight_WS = this_risSample.pointOnLight_WS;
    float Y_p_hat = this_risSample.p_hat;

    const float this_m = 1.f / (NUM_SPATIAL_SAMPLES + 1); // TODO: use better MIS weights (pairwise? use confidence weights?)
    float w_sum = this_m * Y_p_hat * this_risSample.W; // = this_w

    uint numValidSpatialSamples = 0;
    uint sumConfidence = this_risSample.confidence;
    for (uint spatialSampleIdx = 0; spatialSampleIdx < NUM_SPATIAL_SAMPLES; ++spatialSampleIdx)
    {
        const float2 spatialSamplePixelOffset = float2((rng.nextFloat2() - 0.5f) * 2 * SPATIAL_SAMPLE_MAX_RADIUS);
        const int2 spatialSamplePixelIdx = int2(pixelIdx) + int2(round(spatialSamplePixelOffset));
        if (isPixelOutOfBounds(spatialSamplePixelIdx))
        {
            continue;
        }

        const float3 other_surfNor_WS = normalsAndRoughnessTarget[spatialSamplePixelIdx].xyz;
        if (dot(this_surfNor_WS, other_surfNor_WS) < 0.95f)
        {
            continue;
        }

        const uint spatialSampleLinearPixelIdx = spatialSamplePixelIdx.y * renderParams.renderSize.x + spatialSamplePixelIdx.x;
        const RisSample other_risSample = risSamplesIn[spatialSampleLinearPixelIdx];
        if (other_risSample.lightIdx == LIGHT_IDX_INVALID)
        {
            continue;
        }

        const AreaLight other_light = areaLights[other_risSample.lightIdx];

        const float3 other_surfPos_WS = cameraParams.pos_WS + getPrimaryRayDirection(spatialSamplePixelIdx) * linearDepthTarget[spatialSamplePixelIdx];
        const float geomTermJacobian = calcGeomTermJacobian(this_surfPos_WS, other_surfPos_WS, other_risSample.pointOnLight_WS, other_light.normal_WS);

        const float W = other_risSample.W * geomTermJacobian;

        const float m = 1.f / (NUM_SPATIAL_SAMPLES + 1); // TODO: use better MIS weights (pairwise? use confidence weights?)
        const float p_hat = risTargetFunction(other_light, this_surfPos_WS, this_surfNor_WS, other_risSample.pointOnLight_WS);
        const float w = m * p_hat * W;

        w_sum += w;
        if (rng.nextFloat() < w / w_sum)
        {
            Y_lightIdx = other_risSample.lightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = other_risSample.pointOnLight_WS;
        }
        ++numValidSpatialSamples;
        sumConfidence += other_risSample.confidence;
    }

    const float validSpatialSamplesCorrectionFactor = ((NUM_SPATIAL_SAMPLES + 1) / float(numValidSpatialSamples + 1));

    RisSample risSampleOut;
    risSampleOut.lightIdx = Y_lightIdx;
    risSampleOut.pointOnLight_WS = Y_pointOnLight_WS;
    risSampleOut.W = (w_sum / Y_p_hat) * validSpatialSamplesCorrectionFactor;
    risSampleOut.p_hat = Y_p_hat;
    risSampleOut.confidence = min(sumConfidence, RESTIR_MAX_CONFIDENCE);
    risSampleOut.pad0 = 0;
    risSamplesOut[linearPixelIdx] = risSampleOut;
}
