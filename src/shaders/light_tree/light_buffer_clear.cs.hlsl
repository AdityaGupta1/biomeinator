// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

cbuffer LightTreeConstants : REGISTER_B(LIGHT_TREE, CONSTANTS)
{
    uint capacity;
};

RWStructuredBuffer<LightAux> lightAuxOut : REGISTER_U(LIGHT_TREE, LIGHT_AUX_OUT);
RWStructuredBuffer<uint> lightToLeafOut : REGISTER_U(LIGHT_TREE, LIGHT_TO_LEAF_OUT);

[shader("compute")]
[numthreads(LIGHT_BUFFER_CLEAR_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint i = dispatchThreadId.x;
    if (i >= capacity)
    {
        return;
    }

    LightAux aux;
    aux.bboxMin = float3(0, 0, 0);
    aux.flux = 0;
    aux.bboxMax = float3(0, 0, 0);
    aux.pad0 = 0;
    lightAuxOut[i] = aux;

    lightToLeafOut[i] = LEAF_IDX_INVALID;
}
