// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "util/color.hlsli"

SamplerState texSampler : REGISTER_S(POSTPROCESS, TEX_SAMPLER);

struct PsIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 getPathTracingFinalColor(float2 uv)
{
    Texture2D<float4> preTonemappedColorTarget = ResourceDescriptorHeap[renderParams.preTonemappedColorSrvIdx];
    const float3 preTonemappedColor = preTonemappedColorTarget.Sample(texSampler, uv).rgb;
    const float3 tonemappedColor = applyTonemapping(preTonemappedColor);
    return float4(tonemappedColor, 1);
}

float4 psMain(PsIn psIn) : SV_Target
{
    float4 finalColor = getPathTracingFinalColor(psIn.uv);

    if (any(isnan(finalColor)))
    {
        finalColor = float4(100000, 0, 100000, 1);
    }

    return finalColor;
}
