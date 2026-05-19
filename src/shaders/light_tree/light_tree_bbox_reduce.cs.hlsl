// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/light_tree.hlsli"

cbuffer LightTreeConstants : REGISTER_B(LIGHT_TREE, CONSTANTS)
{
    // Sparse capacity of dev_lightAux (Stage 1's pow2-padded buffer). Out-of-range
    // slots hold inverted-infinity sentinels from light_buffer_clear, so they
    // contribute neutrally to the union.
    uint capacity;
};

RWStructuredBuffer<LightAux> lightAuxOut : REGISTER_U(LIGHT_TREE, LIGHT_AUX_OUT);
RWByteAddressBuffer sceneBboxOut : REGISTER_U(LIGHT_TREE, SCENE_BBOX_OUT);

groupshared float3 sMin[LIGHT_TREE_BBOX_REDUCE_WORKGROUP_SIZE];
groupshared float3 sMax[LIGHT_TREE_BBOX_REDUCE_WORKGROUP_SIZE];

[shader("compute")]
[numthreads(LIGHT_TREE_BBOX_REDUCE_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID,
            uint groupIdx : SV_GroupIndex)
{
    const float posInf = asfloat(0x7F800000u);
    const float negInf = asfloat(0xFF800000u);

    float3 lMin = float3(posInf, posInf, posInf);
    float3 lMax = float3(negInf, negInf, negInf);

    const uint i = dispatchThreadId.x;
    if (i < capacity)
    {
        const LightAux aux = lightAuxOut[i];
        lMin = aux.bboxMin;
        lMax = aux.bboxMax;
    }

    sMin[groupIdx] = lMin;
    sMax[groupIdx] = lMax;
    GroupMemoryBarrierWithGroupSync();

    // Workgroup-shared log-step reduce. Loop bound auto-adapts to the
    // workgroup-size define.
    for (uint stride = LIGHT_TREE_BBOX_REDUCE_WORKGROUP_SIZE / 2u; stride > 0u; stride >>= 1u)
    {
        if (groupIdx < stride)
        {
            sMin[groupIdx] = min(sMin[groupIdx], sMin[groupIdx + stride]);
            sMax[groupIdx] = max(sMax[groupIdx], sMax[groupIdx + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIdx == 0u)
    {
        atomicMinFloat(sceneBboxOut, SCENE_BBOX_MIN_OFFSET_BYTES + 0u, sMin[0].x);
        atomicMinFloat(sceneBboxOut, SCENE_BBOX_MIN_OFFSET_BYTES + 4u, sMin[0].y);
        atomicMinFloat(sceneBboxOut, SCENE_BBOX_MIN_OFFSET_BYTES + 8u, sMin[0].z);
        atomicMaxFloat(sceneBboxOut, SCENE_BBOX_MAX_OFFSET_BYTES + 0u, sMax[0].x);
        atomicMaxFloat(sceneBboxOut, SCENE_BBOX_MAX_OFFSET_BYTES + 4u, sMax[0].y);
        atomicMaxFloat(sceneBboxOut, SCENE_BBOX_MAX_OFFSET_BYTES + 8u, sMax[0].z);
    }
}
