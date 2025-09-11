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

#include "global_params.hlsli"
#include "util/color.hlsli"

SamplerState texSampler : REGISTER_S(POSTPROCESS_REGISTER_TEX_SAMPLER, POSTPROCESS_REGISTER_SPACE);

struct PsIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 psMain(PsIn psIn) : SV_Target
{
    Texture2D<float4> albedoTarget = ResourceDescriptorHeap[heapIndices.srv.albedoTargetIdx];
    const float3 albedo = albedoTarget.Sample(texSampler, psIn.uv).rgb;
    return float4(albedo, 1);

    //Texture2D<float4> pathTracingTarget = ResourceDescriptorHeap[heapIndices.srv.pathTracingTargetIdx];
    //const float4 pathTracingColor = pathTracingTarget.Sample(texSampler, psIn.uv);
    //
    //const float3 colorPostTonemap = applyTonemapping(pathTracingColor.rgb);
    //
    //return float4(colorPostTonemap, pathTracingColor.a);
}
