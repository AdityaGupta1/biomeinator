// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "common/global_params.hlsli"
#include "sky/sky_constants.hlsli"
#include "sky/cloud_noise_cache.hlsli"
#include "sky/cloud_reference_noise.hlsli"

[numthreads(64,1,1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<float4> cache = ResourceDescriptorHeap[lutUavIdx];
    const float2 value = cloudNoiseCacheValue(id.xy);
    // Store coefficients at full precision. Runtime queries use integer loads, never filtering.
    cache[id.xy] = float4(value, 0.f, 0.f);
}
