// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_params.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/water_waves.hlsli"

// One dispatch covers every animated instance: thread i is vertex i of the concatenated
// instance vertex ranges and finds its instance by binary search on firstVert
cbuffer WaterDisplaceConstants : REGISTER_B(WATER_DISPLACE, CONSTANTS)
{
    uint numInstances;
    uint numVerts;
    float waveTime;
    uint pad0;
    WaveFadeParams waveFadeParams;
};

StructuredBuffer<WaterDisplaceInstance> instances : REGISTER_T(WATER_DISPLACE, INSTANCES);
RWStructuredBuffer<Vertex> vertsOut : REGISTER_U(WATER_DISPLACE, VERTS_OUT);

[shader("compute")]
[numthreads(WATER_DISPLACE_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint threadIdx = dispatchThreadId.x;
    if (threadIdx >= numVerts)
    {
        return;
    }

    uint lo = 0;
    uint hi = numInstances - 1;
    while (lo < hi)
    {
        const uint mid = (lo + hi + 1) / 2;
        if (instances[mid].firstVert <= threadIdx)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    const WaterDisplaceInstance instance = instances[lo];

    const uint vertIdx = instance.vertsBufferOffset + (threadIdx - instance.firstVert);
    Vertex vert = vertsOut[vertIdx];

    const float restY = round(vert.pos_OS.y - 0.875f) + 0.875f;
    if (abs(vert.pos_OS.y - restY) > 0.11f) // max displacement is 0.125, so 0.11f is used as a safer max for this threshold
    {
        return;
    }

    const float3 restPos_WS = float3(vert.pos_OS.x, restY, vert.pos_OS.z) + float3(instance.transformOffset);
    const float fade = waveFade(restPos_WS, waveFadeParams) * instance.waveScale;
    vert.pos_OS.y = restY + waveHeight(restPos_WS.xz, waveTime) * fade;
    vertsOut[vertIdx] = vert;
}
