// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "sky/atmosphere.hlsli"
#include "sky/sky_constants.hlsli"

// Multi-scattering transfer function Ψms (paper §5.5): per texel, integrate the second-order
// in-scattered luminance L_2ndorder (Eq. 5-6, isotropic phase, unit illuminance, including the
// ground diffuse bounce) and the energy transfer factor f_ms (Eq. 7-8) over uniform sphere
// directions, then sum the infinite scattering orders as a geometric series (Eq. 9-10).
// Depends only on the atmosphere constants, so it's generated once at startup.

static const float isotropicPhase = 1.f / (4.f * M_PI);

[shader("compute")]
[numthreads(SKY_WORKGROUP_SIZE_X, SKY_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= SKY_MULTI_SCATTERING_LUT_SIZE || dispatchThreadId.y >= SKY_MULTI_SCATTERING_LUT_SIZE)
    {
        return;
    }

    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[transmittanceLutSrvIdx];

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / SKY_MULTI_SCATTERING_LUT_SIZE;
    const float muSun = 2.f * uv.x - 1.f;
    const float3 sunDir = float3(sqrt(saturate(1.f - muSun * muSun)), muSun, 0.f);

    // keep the sample point off the exact ground/top spheres, where the ray intersections degenerate
    const float altitude = clamp(uv.y * (atmosphereTopRadius - atmosphereGroundRadius),
                                 1.f,
                                 atmosphereTopRadius - atmosphereGroundRadius - 1.f);
    const float3 rayOrigin = float3(0.f, atmosphereGroundRadius + altitude, 0.f);

    float3 secondOrderLuminance = float3(0.f, 0.f, 0.f);
    float3 transferFactor = float3(0.f, 0.f, 0.f);

    for (uint dirIdx = 0; dirIdx < SKY_MULTI_SCATTERING_NUM_DIRS; ++dirIdx)
    {
        // Fibonacci sphere
        const float goldenAngle = M_PI * (3.f - sqrt(5.f));
        const float dirY = 1.f - 2.f * (dirIdx + 0.5f) / SKY_MULTI_SCATTERING_NUM_DIRS;
        const float dirRadius = sqrt(saturate(1.f - dirY * dirY));
        const float dirAzimuth = goldenAngle * dirIdx;
        const float3 rayDir = float3(cos(dirAzimuth) * dirRadius, dirY, sin(dirAzimuth) * dirRadius);

        const float tGround = raySphereIntersectNearest(rayOrigin, rayDir, atmosphereGroundRadius);
        const float tTop = raySphereIntersectNearest(rayOrigin, rayDir, atmosphereTopRadius);
        const float tMax = tGround >= 0.f ? tGround : tTop;
        const float dt = tMax / SKY_MULTI_SCATTERING_NUM_STEPS;

        float3 luminance = float3(0.f, 0.f, 0.f);
        float3 luminanceFactor = float3(0.f, 0.f, 0.f);
        float3 throughput = float3(1.f, 1.f, 1.f);
        for (uint stepIdx = 0; stepIdx < SKY_MULTI_SCATTERING_NUM_STEPS; ++stepIdx)
        {
            const float t = (stepIdx + 0.5f) * dt;
            const float3 samplePos = rayOrigin + rayDir * t;
            const float sampleRadius = length(samplePos);

            const MediumSample medium = sampleMedium(sampleRadius - atmosphereGroundRadius);
            const float3 sampleTransmittance = exp(-medium.extinction * dt);
            const float3 scattering = medium.rayleighScattering + medium.mieScattering;

            // f_ms integrand (Eq. 8): scattered energy transfer only — no shadowing or phase,
            // those are already accounted for in L_2ndorder
            luminanceFactor += throughput * (scattering - scattering * sampleTransmittance) / max(medium.extinction, 1e-12f);

            // L' integrand (Eq. 6) with unit illuminance and the isotropic phase function
            const float earthShadow =
                raySphereIntersectNearest(samplePos, sunDir, atmosphereGroundRadius) >= 0.f ? 0.f : 1.f;
            const float sampleMuSun = dot(samplePos, sunDir) / sampleRadius;
            const float3 transmittanceToSun = sampleTransmittanceLut(transmittanceLut, lutSampler, sampleRadius, sampleMuSun);
            const float3 scatteredLuminance = earthShadow * transmittanceToSun * scattering * isotropicPhase;
            luminance += throughput * (scatteredLuminance - scatteredLuminance * sampleTransmittance) / max(medium.extinction, 1e-12f);

            throughput *= sampleTransmittance;
        }

        if (tGround >= 0.f)
        {
            const float3 groundPos = rayOrigin + rayDir * tGround;
            const float3 groundNormal = normalize(groundPos);
            const float NdotL = saturate(dot(groundNormal, sunDir));
            const float3 transmittanceToSun =
                sampleTransmittanceLut(transmittanceLut, lutSampler, atmosphereGroundRadius, NdotL);
            luminance += throughput * transmittanceToSun * NdotL * (atmosphereGroundAlbedo * M_INV_PI);
        }

        // integrate against the isotropic phase (Eq. 5 and 7): pu * dΩ = (1/4π)(4π/N) = 1/N
        secondOrderLuminance += luminance / SKY_MULTI_SCATTERING_NUM_DIRS;
        transferFactor += luminanceFactor / SKY_MULTI_SCATTERING_NUM_DIRS;
    }

    // infinite scattering orders as a geometric series (Eq. 9-10)
    const float3 psiMs = secondOrderLuminance / (1.f - transferFactor);

    RWTexture2D<float4> multiScatteringLut = ResourceDescriptorHeap[lutUavIdx];
    multiScatteringLut[dispatchThreadId.xy] = float4(psiMs, 1.f);
}
