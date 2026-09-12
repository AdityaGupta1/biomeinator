// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#ifndef CLOUD_CONFIG
#define CLOUD_CONFIG renderParams.cloud
#endif
#define cloudBase (CLOUD_CONFIG.baseHeight)
#define cloudTop (CLOUD_CONFIG.baseHeight + CLOUD_CONFIG.thickness)
#define cloudPeriod (CLOUD_CONFIG.period)
#define cloudMaxDistance (CLOUD_CONFIG.maxDistance)
#define cloudWind float2(CLOUD_CONFIG.windX, CLOUD_CONFIG.windZ)

float3 cloudFieldUv(float3 p)
{
    return float3(p.x / cloudPeriod, (p.y - cloudBase) / (cloudTop - cloudBase), p.z / cloudPeriod);
}

// Detail removes density from the base envelope, so a zero envelope can be
// skipped without evaluating detail. The light field uses the same erosion.
float cloudDetailDensity(float body, float3 p, Texture3D<float4> noise, SamplerState samplerLinear)
{
    // Both periods divide cloudPeriod, preserving density/light agreement at
    // the spatial cache's wrap seam.
    const float4 medium = noise.SampleLevel(samplerLinear, p / (cloudPeriod / CLOUD_CONFIG.mediumRepeats) + 0.173f, 0);
    // Domain-warp the fine octave so its cellular pattern cannot read as a
    // uniform fringe. Most erosion comes from the larger sub-structure.
    const float4 fine = noise.SampleLevel(samplerLinear, p / (cloudPeriod / CLOUD_CONFIG.fineRepeats) + medium.rgb * CLOUD_CONFIG.warpStrength, 0);
    const float mediumErosion = 1.f - (medium.g * 0.625f + medium.b * 0.25f + medium.a * 0.125f);
    const float fineErosion = 1.f - (fine.g * 0.75f + fine.b * 0.25f);
    const float erosion = mediumErosion * CLOUD_CONFIG.mediumErosion + fineErosion * CLOUD_CONFIG.fineErosion;
    return saturate((body - erosion) / 0.8f);
}
