#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "path_tracing.slang"

RWTexture2D<float4> renderTarget : REGISTER_U(REGISTER_RENDER_TARGET, REGISTER_SPACE_TEXTURES);

#define NUM_SAMPLES_PER_PIXEL 16

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
        ray.Direction = targetPos_WS - cameraParams.pos_WS;
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
