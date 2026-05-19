// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "common/light_tree.hlsli"

// dev_sceneBbox is a 24-byte raw buffer carrying 6 orderable-uint-encoded
// floats: min x/y/z then max x/y/z. Reset to +inf/-inf so the bbox-reduce
// pass's atomic-min/max accumulates the correct envelope from scratch.
RWByteAddressBuffer sceneBboxOut : REGISTER_U(LIGHT_TREE, SCENE_BBOX_OUT);

[shader("compute")]
[numthreads(LIGHT_TREE_SCENE_BBOX_RESET_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint i = dispatchThreadId.x;
    if (i >= 6u)
    {
        return;
    }

    // Slots 0..2 are min (want +inf), slots 3..5 are max (want -inf).
    const float val = (i < 3u) ? asfloat(0x7F800000u) : asfloat(0xFF800000u);
    sceneBboxOut.Store(i * 4u, floatToOrderableUint(val));
}
