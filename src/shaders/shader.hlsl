#include "common_structs.hlsli"
#include "global_params.hlsli"
#include "path_tracing.hlsli"

RWTexture2D<float4> renderTarget : register(u0);

static const uint numSamplesPerPixel = 4;

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 idx = DispatchRaysIndex().xy;
    const float2 size = DispatchRaysDimensions().xy;

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint i = 0; i < numSamplesPerPixel; ++i)
    {
        Payload payload;
        payload.color = float3(1, 1, 1);
        payload.flags = 0;
        payload.flags |= PAYLOAD_FLAG_ALLOW_REFLECTION;
        payload.rng = initRandomSampler3(uint3(idx, sceneParams.frameNumber));

        const float2 jitter = float2(payload.rng.nextFloat(), payload.rng.nextFloat());
        const float3 targetPos_WS = calculateRayTarget(idx + jitter, size);

        RayDesc ray;
        ray.Origin = cameraParams.pos_WS;
        ray.Direction = targetPos_WS - cameraParams.pos_WS;
        ray.TMin = 0.001;
        ray.TMax = 1000;
        
        const bool success = pathTraceRay(ray, payload);
        if (success)
        {
            accumulatedColor += payload.color;
        }
    }

    renderTarget[idx] = float4(accumulatedColor / numSamplesPerPixel, 1);
}