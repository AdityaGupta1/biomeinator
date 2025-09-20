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

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "path_tracing.hlsli"

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const uint2 size = DispatchRaysDimensions().xy;
    const uint linearPixelIdx = pixelIdx.y * size.x + pixelIdx.x;

    RayDesc ray;
    ray.Origin = cameraParams.pos_WS;
    const float3 targetPos_WS = calculateRayTarget(float2(pixelIdx) + cameraParams.jitter, size);
    ray.Direction = normalize(targetPos_WS - cameraParams.pos_WS);
    ray.TMin = 0.001;
    ray.TMax = 1000;

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint sampleIdx = 0; sampleIdx < renderParams.numSamplesPerPixel; ++sampleIdx)
    {
        Payload payload;
        payload.pathWeight = float3(1, 1, 1);
        payload.pathColor = float3(0, 0, 0);
        payload.flags = 0;
        payload.pixelIdx = pixelIdx;
        payload.rng = initRandomSampler4(uint4(constantParams.rngSeed, linearPixelIdx, sampleIdx, renderParams.frameNumber));
        payload.specularHitDistance = 0;

        const bool isFirstSample = (sampleIdx == 0);
        pathTraceRay(ray, payload, isFirstSample);

        accumulatedColor += payload.pathColor;

        if (isFirstSample)
        {
            RWTexture2D<float2> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
            specularHitDistanceTarget[pixelIdx] = payload.specularHitDistance;
        }

    }

    const float3 colorPreTonemap = accumulatedColor / renderParams.numSamplesPerPixel;

    RWTexture2D<float4> pathTracingTarget = ResourceDescriptorHeap[heapIndices.uav.pathTracingTargetIdx];
    pathTracingTarget[pixelIdx] = float4(colorPreTonemap, 1);
}
