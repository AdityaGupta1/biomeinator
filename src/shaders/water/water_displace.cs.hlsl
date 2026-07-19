// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/water_waves.hlsli"

cbuffer WaterDisplaceConstants : REGISTER_B(WATER_DISPLACE, CONSTANTS)
{
    uint vertsBufferOffset; // in verts
    uint vertCount;
    int2 transformOffsetXZ;
    float animTime;
};

RWStructuredBuffer<Vertex> vertsOut : REGISTER_U(WATER_DISPLACE, VERTS_OUT);

[shader("compute")]
[numthreads(WATER_DISPLACE_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= vertCount)
    {
        return;
    }

    const uint vertIdx = vertsBufferOffset + dispatchThreadId.x;
    Vertex vert = vertsOut[vertIdx];

    const float restY = round(vert.pos_OS.y - 0.875f) + 0.875f;
    if (abs(vert.pos_OS.y - restY) > 0.11f) // max displacement is 0.125, so 0.11f is used as a safer max for this threshold
    {
        return;
    }

    const float2 posXZ_WS = vert.pos_OS.xz + float2(transformOffsetXZ);
    vert.pos_OS.y = restY + waveHeight(posXZ_WS, animTime);
    vertsOut[vertIdx] = vert;
}
