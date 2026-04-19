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

    const uint2 key = rcHashEntries.Load2(slot * 8);
    if (all(key == RC_EMPTY_SENTINEL))
    {
        return;
    }

    const uint4 accum = rcAccumulation.Load4(slot * 16);
    if (accum.w > 0)
    {
        const uint currentNumSamples = accum.w;
        const float3 currentRadiance = float3(accum.rgb) / (currentNumSamples * RC_RADIANCE_SCALE);

        const float3 previousRadiance = rcResolved[slot].rgb;
        float previousWeight = rcResolved[slot].w;

        previousWeight *= RC_DECAY;

        const float newWeight = previousWeight + currentNumSamples;
        const float blendFactor = currentNumSamples / newWeight;
        const float3 newRadiance = lerp(previousRadiance, currentRadiance, blendFactor);

        rcResolved[slot] = float4(newRadiance, newWeight);
    }
    else
    {
        rcResolved[slot].w *= RC_DECAY;
    }
}
