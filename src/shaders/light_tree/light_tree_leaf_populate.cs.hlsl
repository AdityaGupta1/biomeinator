// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../../rendering/common/common_registers.h"
#include "../../rendering/common/common_settings.h"
#include "../../rendering/common/common_structs.h"

#include "../common/light_tree.hlsli"

cbuffer LightTreeConstants : REGISTER_B(LIGHT_TREE, CONSTANTS)
{
    uint numAreaLights;
    uint treeLeafBase; // == M - 1, first leaf node index in the perfect-binary tree
};

RWStructuredBuffer<LightAux> lightAuxOut : REGISTER_U(LIGHT_TREE, LIGHT_AUX_OUT);
RWStructuredBuffer<uint> lightToLeafOut : REGISTER_U(LIGHT_TREE, LIGHT_TO_LEAF_OUT);
RWStructuredBuffer<LightTreeNode> lightTreeOut : REGISTER_U(LIGHT_TREE, LIGHT_TREE_OUT);
RWStructuredBuffer<uint> mortonValuesOut : REGISTER_U(LIGHT_TREE, MORTON_VALUES_OUT);

[shader("compute")]
[numthreads(LIGHT_TREE_LEAF_POPULATE_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint s = dispatchThreadId.x;
    // M = nextPow2(numAreaLights) and we dispatch exactly M threads, but the
    // dispatch is rounded up to a multiple of LIGHT_TREE_LEAF_POPULATE_WORKGROUP_SIZE
    // by the host. M is itself >= 256 and pow2, so it is always a multiple of 64.
    // No upper bound guard needed in practice; gate on s < M would require an
    // extra root constant. Skip it.

    if (s < numAreaLights)
    {
        const uint sparseIdx = mortonValuesOut[s];
        const LightAux aux = lightAuxOut[sparseIdx];

        LightTreeNode n;
        n.bboxMin = aux.bboxMin;
        n.flux = aux.flux;
        n.bboxMax = aux.bboxMax;
        n.areaLightIdx = sparseIdx;
        lightTreeOut[treeLeafBase + s] = n;

        lightToLeafOut[sparseIdx] = treeLeafBase + s;
    }
    else
    {
        lightTreeOut[treeLeafBase + s] = makeSentinelLightTreeNode();
        // Skip the scatter; Stage 1's light_buffer_clear already set every
        // dev_lightToLeaf slot to LEAF_IDX_INVALID.
    }
}
