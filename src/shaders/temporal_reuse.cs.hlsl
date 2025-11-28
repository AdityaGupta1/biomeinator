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
#include "util/packing.hlsli"
#include "util/rng.hlsli"

StructuredBuffer<RisSample> risSamplesIn : REGISTER_T(TEMPORAL_REUSE_REGISTER_RIS_SAMPLES_IN, TEMPORAL_REUSE_REGISTER_SPACE);
StructuredBuffer<RisSample> risSamplesPrev : REGISTER_T(TEMPORAL_REUSE_REGISTER_RIS_SAMPLES_PREV, TEMPORAL_REUSE_REGISTER_SPACE);

RWStructuredBuffer<RisSample> risSamplesOut : REGISTER_U(TEMPORAL_REUSE_REGISTER_RIS_SAMPLES_OUT, TEMPORAL_REUSE_REGISTER_SPACE);

struct ReprojectionResult
{
    float score;
    uint2 pixelIdx;

    float3 this_surfPos_WS;
    float3 this_surfNor_WS;

    float3 reproj_surfPos_WS;
};

ReprojectionResult reproject(uint2 pixelIdx)
{
    ReprojectionResult result;
    result.score = 0.f;

    Texture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.srv.motionTargetIdx];
    const float2 motionPixels = motionTarget[pixelIdx] * renderParams.renderSize;
    const float2 reprojectedPixelPos = float2(pixelIdx) + cameraParams.jitter + motionPixels; // want to find the closest pixel to the fractional pixel pos where this world pos was last frame

    const int2 minCornerPixelIdx = int2(floor(reprojectedPixelPos)) - 1;
    const int2 maxCornerPixelIdx = minCornerPixelIdx + 2;
    //const int2 minCornerPixelIdx = pixelIdx;
    //const int2 maxCornerPixelIdx = pixelIdx;
    if (isPixelOutOfBounds(minCornerPixelIdx) && isPixelOutOfBounds(maxCornerPixelIdx))
    {
        return result;
    }

    Texture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float3 primaryRayDirection = getPrimaryRayDirection(pixelIdx);
    const float3 this_surfPos_WS = cameraParams.pos_WS + primaryRayDirection * linearDepthTarget[pixelIdx];

    Texture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.srv.normalsAndRoughnessTargetIdx];
    const float3 this_surfNor_WS = normalsAndRoughnessTarget[pixelIdx].xyz;

    Texture2D<uint2> prevDepthAndNormalTarget = ResourceDescriptorHeap[heapIndices.srv.prevDepthAndNormalTargetIdx];
    for (int y = minCornerPixelIdx.y; y <= maxCornerPixelIdx.y; ++y)
    {
        for (int x = minCornerPixelIdx.x; x <= maxCornerPixelIdx.x; ++x)
        {
            const int2 reprojectCandidatePixelIdx = int2(x, y);
            if (isPixelOutOfBounds(reprojectCandidatePixelIdx))
            {
                continue;
            }

            const uint2 packedReprojDepthAndNormal = prevDepthAndNormalTarget[reprojectCandidatePixelIdx];
            const float reproj_depth = asfloat(packedReprojDepthAndNormal.x);
            const float3 reproj_surfNor_WS = octDecode(packedReprojDepthAndNormal.y);

            const float3 reproj_surfPos_WS = cameraParams.prevPos_WS + getPrevPrimaryRayDirection(reprojectCandidatePixelIdx) * reproj_depth;
            const float dist = distance(this_surfPos_WS, reproj_surfPos_WS);

            const float positionReprojectionScore = max(0.2f - dist, 0.f) / 0.2f;
            const float normalReprojectionScore = max((dot(this_surfNor_WS, reproj_surfNor_WS) - 0.9f), 0.f) / 0.1f;

            const float candidateReprojectionScore = positionReprojectionScore * normalReprojectionScore;
            if (candidateReprojectionScore > result.score)
            {
                result.score = candidateReprojectionScore;
                result.pixelIdx = reprojectCandidatePixelIdx;

                result.this_surfPos_WS = this_surfPos_WS;
                result.this_surfNor_WS = this_surfNor_WS;

                result.reproj_surfPos_WS = reproj_surfPos_WS;
            }
        }
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

    //{
    //    uint2 reprojectedPixelIdx;
    //    const ReprojectionResult reprojResult = reproject(pixelIdx);
    //    debugTexture()[pixelIdx] = float4(reprojResult.score.xxx, 1);
    //}

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const RisSample this_risSample = risSamplesIn[linearPixelIdx];
    if (this_risSample.lightIdx == LIGHT_IDX_INVALID)
    {
        risSamplesOut[linearPixelIdx] = this_risSample;
        return;
    }

    const ReprojectionResult reprojResult = reproject(pixelIdx);

    if (reprojResult.score < 0.01f)
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

    // TODO: MIS with confidence weights (cap at 20 probably, and maybe multiply by reprojectionScore?)
    const float this_m = 0.5f;
    const float reproj_m = 0.5f;

    const AreaLight reproj_light = areaLights[reproj_risSample.lightIdx];
    const float reproj_p_hat = risTargetFunction(reproj_light, reprojResult.this_surfPos_WS, reprojResult.this_surfNor_WS, reproj_risSample.pointOnLight_WS);
    const float geomTermJacobian = calcGeomTermJacobian(reprojResult.this_surfPos_WS, reprojResult.reproj_surfPos_WS, reproj_risSample.pointOnLight_WS, reproj_light.normal_WS);
    const float reproj_W = reproj_risSample.W * geomTermJacobian;

    const float this_w = this_m * this_risSample.p_hat * this_risSample.W;
    const float reproj_w = reproj_m * reproj_p_hat * reproj_W;
    const float w_sum = this_w + reproj_w;

    RandomSampler rng = initRandomSampler(constantParams.rngSeed, 44721359, linearPixelIdx, renderParams.frameNumber);

    RisSample risSampleOut;
    float Y_p_hat;
    const bool chooseThis = (rng.nextFloat() < this_w / w_sum);
    if (chooseThis)
    {
        risSampleOut.lightIdx = this_risSample.lightIdx;
        risSampleOut.pointOnLight_WS = this_risSample.pointOnLight_WS;
        Y_p_hat = this_risSample.p_hat;
    }
    else
    {
        risSampleOut.lightIdx = reproj_risSample.lightIdx;
        risSampleOut.pointOnLight_WS = reproj_risSample.pointOnLight_WS;
        Y_p_hat = reproj_p_hat;
    }
    risSampleOut.W = w_sum / Y_p_hat;
    risSampleOut.p_hat = Y_p_hat;
    risSampleOut.pad0 = risSampleOut.pad1 = 0;
    risSamplesOut[linearPixelIdx] = risSampleOut;
}
