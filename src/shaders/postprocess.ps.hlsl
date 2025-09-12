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

SamplerState texSampler : REGISTER_S(POSTPROCESS_REGISTER_TEX_SAMPLER, POSTPROCESS_REGISTER_SPACE);

struct PsIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 getPathTracingFinalColor(float2 uv)
{
    Texture2D<float4> pathTracingTarget = ResourceDescriptorHeap[heapIndices.srv.pathTracingTargetIdx];
    const float4 pathTracingColor = pathTracingTarget.Sample(texSampler, uv);

    const float3 tonemappedColor = applyTonemapping(pathTracingColor.rgb);
    return tonemappedColor;
}

float4 psMain(PsIn psIn) : SV_Target
{
    float3 finalColor = 0;

    switch ((DebugView) renderParams.debugView)
    {
        case DebugView::DIFFUSE_ALBEDO:
        {
            Texture2D<float4> diffuseAlbedoTarget = ResourceDescriptorHeap[heapIndices.srv.diffuseAlbedoTargetIdx];
            finalColor = diffuseAlbedoTarget.Sample(texSampler, psIn.uv).rgb;
            break;
        }
        case DebugView::OFF:
        default:
        {
            finalColor = getPathTracingFinalColor(psIn.uv);
            break;
        }
    }

    return float4(finalColor, 1);
}
