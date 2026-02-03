/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
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
    const float4 preTonemappedColor = preTonemappedColorTarget.Sample(texSampler, uv);

    const float3 tonemappedColor = applyTonemapping(preTonemappedColor.rgb);
    return float4(tonemappedColor, 1);
}

float4 getDebugOutputColor(float2 uv)
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

    if (debugParams.debugOutputSrvIdx == heapIndices.srv.pathTracingTargetIdx)
    {
        debugColor.rgb = applyTonemapping(debugColor.rgb);
    }

    debugColor.rgb *= debugParams.debugOutputScale;
    return debugColor;
}

float4 psMain(PsIn psIn) : SV_Target
{
    float4 finalColor = 0;

    if (debugParams.debugOutputSrvIdx == ~0u)
    {
        finalColor = getPathTracingFinalColor(psIn.uv);
    }
    else
    {
        finalColor = getDebugOutputColor(psIn.uv);
    }

    if (any(isnan(finalColor)))
    {
        finalColor = float4(100000, 0, 100000, 1);
    }

    return finalColor;
}
