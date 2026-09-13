// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#include "sky/atmosphere.hlsli"
#include "sky/sun_sampling.hlsli"
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
    RandomNumberGenerator rng = initRng(id.x, id.y, id.z, sunSampleFrame);
    const float3 sun = sampleSunDirection(computeSunDir_WS(animTime), rng);
    // One randomized solar direction per cache texel. Use midpoint density
    // quadrature inside that ray: independent position jitter in this coarse
    // cache persists as spatial speckle in the multiple-scattering estimate.
    output[id] = cloudRayOpticalDepth(pos, sun, shape, cloudNoiseSampler,
        cloudCoverage, cloudDensity);
}
