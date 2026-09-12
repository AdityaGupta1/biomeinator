// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#include "sky/cloud_reference_noise.hlsli"

[numthreads(4,4,4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.y != 0u) return;
    RWTexture3D<float4> output = ResourceDescriptorHeap[lutUavIdx];
    const float2 p = ((float2(id.x,id.z)+0.5f)/1024.f-0.5f)*16.f;
    output[id] = float4(cloudReferenceWarp(p,cloud.warpScale,cloud.warpDetail,cloud.warpRoughness,cloud.warpTime+animTime*cloud.warpSpeed),0.f,0.f);
}
