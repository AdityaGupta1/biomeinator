// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
#include "util/color.hlsli"
#include "util/ray.hlsli"

SamplerState texSampler : REGISTER_S(POSTPROCESS, TEX_SAMPLER);

struct PsIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 reconstructWorldPos(float2 uv)
{
    Texture2D<float> linearDepthTex = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float linearDepth = linearDepthTex.SampleLevel(texSampler, uv, 0);

    const uint2 pixelIdx = uint2(uv * float2(renderParams.renderSize));
    const float3 rayDir = getPrimaryRayDirection(pixelIdx);

    return evalRayPos(cameraParams.pos_WS, rayDir, linearDepth);
}

float4 getDebugColor(float2 uv)
{
    float4 debugColor = 0;

    switch (debugParams.debugOutputNumChannels)
    {
        case 4:
        {
            Texture2D<float4> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = debugTexture.Sample(texSampler, uv).rgba;
            break;
        }
        case 3:
        {
            Texture2D<float4> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = float4(debugTexture.Sample(texSampler, uv).rgb, 1);
            break;
        }
        case 2:
        {
            Texture2D<float2> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = float4(debugTexture.Sample(texSampler, uv).rg, 0, 1);
            break;
        }
        case 1:
        {
            Texture2D<float> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = float4(debugTexture.Sample(texSampler, uv).rrr, 1);
            break;
        }
    }

    return debugColor;
}

float4 psMain(PsIn psIn) : SV_Target
{
    float4 debugColor = getDebugColor(psIn.uv);

    if (debugParams.debugViewApplyTonemap)
    {
        debugColor.rgb = applyTonemapping(debugColor.rgb);
    }

    debugColor.rgb *= debugParams.debugOutputScale;

    return debugColor;
}
