// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "global_params.hlsli"
#include "radiance_cache.hlsli"

RWByteAddressBuffer rcHashEntries : REGISTER_U(RC, HASH_ENTRIES);
RWByteAddressBuffer rcAccumulation : REGISTER_U(RC, ACCUMULATION);
RWStructuredBuffer<float4> rcResolved : REGISTER_U(RC, RESOLVED);

[shader("compute")]
[numthreads(RC_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadId.x;
    if (slot >= RC_TABLE_SIZE)
    {
        return;
    }

    // clear accumulation buffer
    rcAccumulation.Store4(slot * 16, uint4(0, 0, 0, 0));

    // evict resolved entries that have fallen below the stale weight threshold
    const uint2 key = rcHashEntries.Load2(slot * 8);
    if (any(key != RC_EMPTY_SENTINEL))
    {
        if (rcResolved[slot].w < RC_STALE_WEIGHT_THRESHOLD)
        {
            rcHashEntries.Store2(slot * 8, uint2(RC_EMPTY_SENTINEL, RC_EMPTY_SENTINEL));
            rcResolved[slot] = float4(0, 0, 0, 0);
        }
    }
}
