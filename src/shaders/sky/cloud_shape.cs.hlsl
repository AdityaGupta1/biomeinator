// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#include "sky/cloud_reference_noise.hlsli"

[numthreads(4,4,4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    Texture3D<float4> noise = ResourceDescriptorHeap[cloudNoiseIdx];
    if (id.y != 0u) return;
    RWTexture3D<float> output = ResourceDescriptorHeap[lutUavIdx];
    const float2 p = ((float2(id.x,id.z)+0.5f)/1024.f-0.5f)*16.f;
    // The graph adds Noise Color at strength 1; it does NOT subtract 0.5.
    output[id] = cloudSmoothF1(p+noise.Load(int4(id,0)).rg*cloud.warpStrength,cloud.voronoiSmoothness,cloud.voronoiRandomness,cloud.voronoiTime+animTime*cloud.voronoiSpeed);
}
