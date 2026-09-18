// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "light/fog_density.hlsli"
#include "materials/water.hlsli"
#include "sky/cloud_traversal.hlsli"
#include "sky/sky_lighting.hlsli"
#include "util/sampling.hlsli"

// Analytic optical depth through occupied cells. Shadow-type query: travel inside the layer is
// limited to the shadow distance, and accumulation stops once the result is effectively opaque.
float cloudOpticalDepth(const float3 origin_WS, const float3 dir, const float maxDistance)
{
    CloudTraversal state = beginCloudTraversal(origin_WS, dir, maxDistance, renderParams.cloudSettings.shadowDistance);
    float depth = 0.f;
    float2 interval;
    [loop] while (nextCloudInterval(state, interval))
    {
        depth += renderParams.cloudSettings.extinction * (interval.y - interval.x);
        if (depth > 12.f)
        {
            break;
        }
    }
    return depth;
}

float cloudTransmittance(const float3 origin_WS, const float3 dir, const float maxDistance)
{
    return exp(-cloudOpticalDepth(origin_WS, dir, maxDistance));
}

// Fraction of one sampled sun direction's illuminance scattered towards the viewer: single
// scattering through the phase function, plus two softer attenuation terms standing in for
// multiple scattering, which is not traced as further bounces.
float cloudSunScattering(const float cosAngle, const float sunOpticalDepth)
{
    const CloudSettings c = renderParams.cloudSettings;
    const float single = henyeyGreensteinPhase(cosAngle, c.phaseG) * exp(-sunOpticalDepth);
    const float multi = c.multiScatterStrength
        * (0.45f * exp(-0.35f * sunOpticalDepth) + 0.2f * exp(-0.12f * sunOpticalDepth)) / (4.f * M_PI);
    return single + multi;
}

float3 cloudAmbientLight()
{
    return renderParams.cloudSettings.ambient * getSkyColor(float3(0.f, 1.f, 0.f));
}

// DLSS guide color for a sky ray: unshadowed cloud color blended with the sky by the segment's
// analytic transmittance, so the lighting sample count and RNG can never add noise to the guide.
float3 cloudGuideColor(const float3 dir, const float3 skyColor, const float transmittance)
{
    if (transmittance == 1.f)
    {
        return skyColor;
    }
    const CloudSettings c = renderParams.cloudSettings;
    const float3 sunDir = getSunDir_WS();
    const float3 sunLight = getAttenuatedSunIlluminance(sunDir, c.baseHeight + 0.5f * c.thickness);
    const float3 unshadowedColor = cloudAmbientLight() + sunLight * cloudSunScattering(dot(dir, sunDir), 0.f);
    return lerp(unshadowedColor, skyColor, transmittance);
}

struct CloudResult
{
    float3 radiance;
    float transmittance;
};

// Integrates the occupied intervals of a segment. Opacity is analytic per interval; the samples
// only estimate in-scattering, stratified over the interval's extinction-weighted depth so an
// opaque cloud still lights its visible skin.
// fogDistance and waterDistance are the lengths of those media in front of the clouds (0 if none).
CloudResult integrateClouds(const float3 origin_WS, const float3 dir, CloudTraversal state, const float fogDistance,
    const float waterDistance, const bool scatter, inout RandomNumberGenerator rng)
{
    CloudResult result;
    result.radiance = 0.f;
    result.transmittance = 1.f;
    float2 interval;
    if (!nextCloudInterval(state, interval))
    {
        return result;
    }

    const CloudSettings c = renderParams.cloudSettings;
    const float3 ambient = cloudAmbientLight();
    [loop] do
    {
        const float intervalTransmittance = exp(-c.extinction * (interval.y - interval.x));
        if (scatter)
        {
            float3 lighting = 0.f;
            [loop] for (uint i = 0; i < c.samples; ++i)
            {
                const float u = (float(i) + rng.nextFloat()) / float(c.samples);
                const float sampleT = interval.x - log(max(1e-7f, 1.f - u * (1.f - intervalTransmittance))) / c.extinction;
                const float3 samplePos_WS = origin_WS + dir * sampleT;
                const float3 sunDir = sampleSunDirection(getSunDir_WS(), rng);
                const float3 sunLight =
                    getAttenuatedSunIlluminance(sunDir, samplePos_WS.y + float(cameraParams.globalInstanceOffset.y));
                float3 source = ambient;
                if (any(sunLight > 0.f))
                {
                    const float sunOpticalDepth = cloudOpticalDepth(samplePos_WS, sunDir, cloudUnboundedDistance);
                    source += sunLight * cloudSunScattering(dot(dir, sunDir), sunOpticalDepth);
                }
                const float fogT = fogDistance > 0.f ? computeFogTransmittance(origin_WS, dir, min(sampleT, fogDistance)) : 1.f;
                const float3 waterT = computeWaterAbsorption(min(sampleT, waterDistance));
                lighting += source * fogT * waterT;
            }
            result.radiance += result.transmittance * (1.f - intervalTransmittance) * lighting / float(c.samples);
        }
        result.transmittance *= intervalTransmittance;
        if (result.transmittance < 0.002f)
        {
            break;
        }
    } while (nextCloudInterval(state, interval));
    return result;
}
