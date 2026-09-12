// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_lighting.hlsli"
#include "sky/clouds.hlsli"

cbuffer CloudViewOutput : REGISTER_B(SKY, CONSTANTS) { uint cloudViewUav; };

[numthreads(8,8,1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[cloudViewUav];
    uint width, height;
    output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    const float2 uv = (float2(id.xy) + 0.5f) / float2(width, height);
    const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);
    const float aspect = float(renderParams.renderSize.x) / renderParams.renderSize.y;
    const float3 dir = normalize(cameraParams.forward_WS +
        cameraParams.right_WS * ndc.x * cameraParams.tanHalfFovY * aspect +
        cameraParams.up_WS * ndc.y * cameraParams.tanHalfFovY);
    output[id.xy] = integrateClouds(cameraParams.pos_WS, dir, renderParams.cloudSteps);
}
