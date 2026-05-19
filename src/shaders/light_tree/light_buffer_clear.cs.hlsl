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

    // Inverted-infinity sentinel bbox so that Stage 2's bottom-up bbox union
    // (min over child mins, max over child maxes) absorbs bogus leaves without
    // a flux-mask branch. Combined with flux == 0 this also keeps the HIS
    // weight at zero, so a bogus leaf can never be selected.
    const float posInf = asfloat(0x7F800000u);
    const float negInf = asfloat(0xFF800000u);
    LightAux aux;
    aux.bboxMin = float3(posInf, posInf, posInf);
    aux.flux = 0.f;
    aux.bboxMax = float3(negInf, negInf, negInf);
    aux.pad0 = 0; // DXC validator rejects struct UAV stores with undef members
    lightAuxOut[i] = aux;

    lightToLeafOut[i] = LEAF_IDX_INVALID;
}
