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

SamplerState texSampler : REGISTER_S(POSTPROCESS_REGISTER_TEX_SAMPLER, POSTPROCESS_REGISTER_SPACE);

// TODO: remove pos?
// TODO: if that's not possible, maybe define VsOut and PsIn as the same struct in the same place?
struct PsIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 psMain(PsIn psIn) : SV_Target
{
    // TODO: make texture float4 instead of unorm
    Texture2D<float4> tex = ResourceDescriptorHeap[heapIndices.srv.pathTracingTargetIdx]; // TODO: add SRV on host side
    float4 texColor = tex.Sample(texSampler, psIn.uv);

    // TODO: tonemapping

    return texColor;
}
