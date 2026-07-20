// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "sky/atmosphere.hlsli"
#include "sky/sky_constants.hlsli"

// Per-wavelength transmittance from a point at radius r to the atmosphere top along a ray with
// zenith cosine mu. Depends only on the atmosphere constants, so it's generated once at startup.

[shader("compute")]
[numthreads(SKY_WORKGROUP_SIZE_X, SKY_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= SKY_TRANSMITTANCE_LUT_WIDTH || dispatchThreadId.y >= SKY_TRANSMITTANCE_LUT_HEIGHT)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(SKY_TRANSMITTANCE_LUT_WIDTH, SKY_TRANSMITTANCE_LUT_HEIGHT);
    float r, mu;
    transmittanceLutUvToRMu(uv, r, mu);

    const float3 rayOrigin = float3(0.f, r, 0.f);
    const float3 rayDir = float3(sqrt(saturate(1.f - mu * mu)), mu, 0.f);
    const float tTop = raySphereIntersectNearest(rayOrigin, rayDir, atmosphereTopRadius);
    const float dt = tTop / SKY_TRANSMITTANCE_LUT_NUM_STEPS;

    float3 opticalDepth = float3(0.f, 0.f, 0.f);
    for (uint stepIdx = 0; stepIdx < SKY_TRANSMITTANCE_LUT_NUM_STEPS; ++stepIdx)
    {
        const float t = (stepIdx + 0.5f) * dt;
        const float altitude = length(rayOrigin + rayDir * t) - atmosphereGroundRadius;
        opticalDepth += sampleMedium(altitude).extinction * dt;
    }

    RWTexture2D<float4> transmittanceLut = ResourceDescriptorHeap[lutUavIdx];
    transmittanceLut[dispatchThreadId.xy] = float4(exp(-opticalDepth), 1.f);
}
