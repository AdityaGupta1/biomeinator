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
    float time;
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

    // Rest Y of every displaceable vert is k + 7/8; all other water verts sit at integer Y,
    // which reconstructs exactly 0.125 from the nearest rest position. The classification
    // threshold (0.11) sits strictly between the max wave amplitude (0.1) and 0.125, so
    // peak-displacement verts have float slack and integer verts still fail cleanly.
    const float restY = round(vert.pos_OS.y - 0.875f) + 0.875f;
    if (abs(vert.pos_OS.y - restY) > 0.11f)
    {
        return;
    }

    // chunk transforms are pure integer translations, so world XZ is exact float math,
    // giving identical inputs for shared boundary verts across chunks (no seams)
    const float2 posXZ_WS = vert.pos_OS.xz + float2(transformOffsetXZ);
    vert.pos_OS.y = restY + waveHeight(posXZ_WS, time);
    vertsOut[vertIdx] = vert;
}
