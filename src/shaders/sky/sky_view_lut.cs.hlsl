// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "sky/atmosphere.hlsli"
#include "sky/sky_constants.hlsli"

// Lat/long map of single-scattered sky luminance around the camera (Eq. 1-4 of the paper), for
// unit sun illuminance — dome_light.hlsli multiplies by the actual solar illuminance. The
// sun-shadowing transmittance T(x, sun) is a transmittance LUT fetch instead of a nested march.
// Regenerated every frame (the sun moves with animTime; also tracks camera altitude).

[shader("compute")]
[numthreads(SKY_WORKGROUP_SIZE_X, SKY_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= SKY_VIEW_LUT_WIDTH || dispatchThreadId.y >= SKY_VIEW_LUT_HEIGHT)
    {
        return;
    }

    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[transmittanceLutSrvIdx];

    const float3 sunDir_WS = computeSunDir_WS(animTime);

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(SKY_VIEW_LUT_WIDTH, SKY_VIEW_LUT_HEIGHT);
    const float3 rayDir = skyViewUvToDir(uv, sunDir_WS);

    const float r = atmosphereRadiusForCameraY(cameraY);
    const float3 rayOrigin = float3(0.f, r, 0.f);

    const float tGround = raySphereIntersectNearest(rayOrigin, rayDir, atmosphereGroundRadius);
    const float tTop = raySphereIntersectNearest(rayOrigin, rayDir, atmosphereTopRadius);
    const float tMax = tGround >= 0.f ? tGround : tTop;
    const float dt = tMax / SKY_VIEW_LUT_NUM_STEPS;

    const float cosTheta = dot(rayDir, sunDir_WS);
    const float rayleighPhaseValue = rayleighPhase(cosTheta);
    const float miePhaseValue = miePhase(cosTheta);

    float3 luminance = float3(0.f, 0.f, 0.f);
    float3 throughput = float3(1.f, 1.f, 1.f);
    for (uint stepIdx = 0; stepIdx < SKY_VIEW_LUT_NUM_STEPS; ++stepIdx)
    {
        const float t = (stepIdx + 0.5f) * dt;
        const float3 samplePos = rayOrigin + rayDir * t;
        const float sampleRadius = length(samplePos);

        const MediumSample medium = sampleMedium(sampleRadius - atmosphereGroundRadius);
        const float3 sampleTransmittance = exp(-medium.extinction * dt);

        // the transmittance LUT only covers rays that don't hit the ground, so the planet's own
        // shadow needs an explicit visibility check
        const float earthShadow = raySphereIntersectNearest(samplePos, sunDir_WS, atmosphereGroundRadius) >= 0.f ? 0.f : 1.f;
        const float muSun = dot(samplePos, sunDir_WS) / sampleRadius;
        const float3 transmittanceToSun = sampleTransmittanceLut(transmittanceLut, lutSampler, sampleRadius, muSun);

        const float3 phaseTimesScattering =
            medium.rayleighScattering * rayleighPhaseValue + medium.mieScattering * miePhaseValue;
        const float3 scatteredLuminance = earthShadow * transmittanceToSun * phaseTimesScattering;

        // analytic integration of the scattered luminance over the segment (Hillaire's
        // energy-conserving form), instead of a plain midpoint sum
        luminance += throughput * (scatteredLuminance - scatteredLuminance * sampleTransmittance) / max(medium.extinction, 1e-12f);
        throughput *= sampleTransmittance;
    }

    if (tGround >= 0.f)
    {
        const float3 groundPos = rayOrigin + rayDir * tGround;
        const float3 groundNormal = normalize(groundPos);
        const float NdotL = saturate(dot(groundNormal, sunDir_WS));
        const float3 transmittanceToSun =
            sampleTransmittanceLut(transmittanceLut, lutSampler, atmosphereGroundRadius, NdotL);
        luminance += throughput * transmittanceToSun * NdotL * (atmosphereGroundAlbedo * M_INV_PI);
    }

    RWTexture2D<float4> skyViewLut = ResourceDescriptorHeap[lutUavIdx];
    skyViewLut[dispatchThreadId.xy] = float4(luminance, 1.f);
}
