// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#include "sky/atmosphere.hlsli"
#define CLOUD_CONFIG cloud
#include "sky/cloud_model.hlsli"

[numthreads(4,4,4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    Texture3D<float> shape = ResourceDescriptorHeap[cloudShapeIdx];
    Texture3D<float4> noise = ResourceDescriptorHeap[cloudNoiseIdx];
    RWTexture3D<float> output = ResourceDescriptorHeap[lutUavIdx];
    float3 uv = (float3(id) + 0.5f) / float3(128,32,128);
    float3 pos = float3(uv.x * cloudPeriod, lerp(cloudBase, cloudTop, uv.y), uv.z * cloudPeriod);
    const float3 sun = computeSunDir_WS(animTime);
    float depth = 0.f;
    if (sun.y > 0.f)
    {
        float length = min(12000.f, (cloudTop - pos.y) / max(sun.y, 0.025f));
        float ds = length / cloud.lightSteps;
        [loop] for (uint i=0; i<uint(cloud.lightSteps); ++i)
        {
            float3 p = pos + sun * ((i + 0.5f)*ds);
            float body = shape.SampleLevel(cloudNoiseSampler, cloudFieldUv(p), 0);
            if (body > 0.f)
                depth += cloudDetailDensity(body,p,noise,cloudNoiseSampler) * ds;
        }
    }
    output[id] = depth * cloudDensity;
}
