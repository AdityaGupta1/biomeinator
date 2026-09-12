// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#define CLOUD_CONFIG cloud
#include "sky/cloud_model.hlsli"

[numthreads(4,4,4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    Texture3D<float4> noise = ResourceDescriptorHeap[cloudNoiseIdx];
    RWTexture3D<float> output = ResourceDescriptorHeap[lutUavIdx];
    const float3 uv = (float3(id) + 0.5f) / float3(128,32,128);
    const float h = uv.y;
    // Independent scales: kilometer-scale weather chooses where clouds exist;
    // Perlin-Worley billows build structure inside each weather mass.
    const float weatherNoise = noise.SampleLevel(cloudNoiseSampler, float3(uv.x * cloud.weatherScale, 0.37f, uv.z * cloud.weatherScale), 0).r;
    const float threshold = lerp(0.6f, 0.36f, cloudCoverage);
    const float weather = smoothstep(threshold - cloud.weatherSoftness, threshold + cloud.weatherSoftness, weatherNoise);
    const float4 n = noise.SampleLevel(cloudNoiseSampler, float3(uv.x * cloud.billowScale, h * 0.5f, uv.z * cloud.billowScale), 0);
    const float billows = lerp(n.g * 0.7f + n.b * 0.3f, n.r, cloud.perlinWeight);
    const float body = weather * saturate((billows - cloud.shapeThreshold) * cloud.shapeGain);
    const float top = lerp(1.f - cloud.topVariance, 1.f, n.r);
    const float profile = smoothstep(0.f, cloud.bottomFade, h) * (1.f - smoothstep(top * cloud.topStart, top, h));
    output[id] = body * profile;
}
