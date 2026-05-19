// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/light_tree.hlsli"

cbuffer LightTreeConstants : REGISTER_B(LIGHT_TREE, CONSTANTS)
{
    // Index of the first node at the topmost level this dispatch writes.
    // For a perfect-binary tree, levelOffset = 2^topLevel - 1.
    uint levelOffset;
    // Number of nodes at the topmost level being written = 2^topLevel.
    uint levelCount;
    // 1: read 2 children (one level below), write 1 parent.
    // 2: read 4 grandchildren (two levels below), write 2 intermediate children + 1 parent.
    uint depth;
};

RWStructuredBuffer<LightTreeNode> lightTreeOut : REGISTER_U(LIGHT_TREE, LIGHT_TREE_OUT);

[shader("compute")]
[numthreads(LIGHT_TREE_INTERNAL_LEVELS_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint t = dispatchThreadId.x;
    if (t >= levelCount)
    {
        return;
    }

    const uint nodeIdx = levelOffset + t;
    const uint childL = 2u * nodeIdx + 1u;
    const uint childR = 2u * nodeIdx + 2u;

    if (depth == 1u)
    {
        const LightTreeNode l = lightTreeOut[childL];
        const LightTreeNode r = lightTreeOut[childR];
        lightTreeOut[nodeIdx] = unionLightTreeNodes(l, r);
    }
    else
    {
        // depth == 2 — fuse two bottom-up levels into one dispatch. Each
        // thread owns a disjoint (parent, 2 children, 4 grandchildren) tuple
        // so no inter-thread write contention.
        const uint gLL = 2u * childL + 1u;
        const uint gLR = 2u * childL + 2u;
        const uint gRL = 2u * childR + 1u;
        const uint gRR = 2u * childR + 2u;

        const LightTreeNode aLL = lightTreeOut[gLL];
        const LightTreeNode aLR = lightTreeOut[gLR];
        const LightTreeNode aRL = lightTreeOut[gRL];
        const LightTreeNode aRR = lightTreeOut[gRR];

        const LightTreeNode l = unionLightTreeNodes(aLL, aLR);
        const LightTreeNode r = unionLightTreeNodes(aRL, aRR);

        lightTreeOut[childL] = l;
        lightTreeOut[childR] = r;
        lightTreeOut[nodeIdx] = unionLightTreeNodes(l, r);
    }
}
