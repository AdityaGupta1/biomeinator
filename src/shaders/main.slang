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

#include "path_tracing.slang"

RWTexture2D<float4> renderTarget : REGISTER_U(REGISTER_RENDER_TARGET, REGISTER_SPACE_TEXTURES);

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const float2 size = DispatchRaysDimensions().xy;

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint sampleIdx = 0; sampleIdx < NUM_SAMPLES_PER_PIXEL; ++sampleIdx)
    {
        Payload payload;
        payload.pathWeight = float3(1, 1, 1);
        payload.pathColor = float3(0, 0, 0);
        payload.flags = 0;
        payload.rng = initRandomSampler4(uint4(pixelIdx, sampleIdx, sceneParams.frameNumber));

        const float2 jitter = float2(payload.rng.nextFloat(), payload.rng.nextFloat());
        const float3 targetPos_WS = calculateRayTarget(pixelIdx + jitter, size);

        RayDesc ray;
        ray.Origin = cameraParams.pos_WS;
        ray.Direction = normalize(targetPos_WS - cameraParams.pos_WS);
        ray.TMin = 0.001;
        ray.TMax = 1000;

        bool pathHadContribution = pathTraceRay(ray, payload);
        if (pathHadContribution)
        {
            accumulatedColor += payload.pathColor;
        }
    }

    renderTarget[pixelIdx] = float4(accumulatedColor / NUM_SAMPLES_PER_PIXEL, 1);
}
