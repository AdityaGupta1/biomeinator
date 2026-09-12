// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#include "sky/atmosphere.hlsli"
#define CLOUD_CONFIG cloud
#define CLOUD_TIME animTime
#include "sky/cloud_model.hlsli"

[numthreads(4,4,4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    Texture3D<float> shape = ResourceDescriptorHeap[cloudShapeIdx];
    RWTexture3D<float> output = ResourceDescriptorHeap[lutUavIdx];
    float3 uv = (float3(id) + 0.5f) / float3(128,32,128);
    float3 pos = cloudFromField(float3(uv.x * cloudPeriod, lerp(cloudBase, cloudTop, uv.y), uv.z * cloudPeriod));
    const float3 sun = computeSunDir_WS(animTime);
    float depth = 0.f;
    // Trace either layer exit, continuously through horizontal sunlight. The old
    // sun.y > 0 branch reset every cached shadow to zero at sunset.
    {
        float exitHeight = sun.y >= 0.f ? cloudTop - pos.y : pos.y - cloudBase;
        float length = min(12000.f, exitHeight / max(abs(sun.y), 1.e-6f));
        float ds = length / cloud.lightSteps;
        [loop] for (uint i=0; i<uint(cloud.lightSteps); ++i)
        {
            float3 p = pos + sun * ((i + 0.5f)*ds);
            float body = cloudBodyMargin(p, shape, cloudNoiseSampler, cloudCoverage);
            if (body > 0.f)
                depth += cloudDetailDensity(body,p) * ds;
        }
    }
    output[id] = depth * cloudDensity;
}
