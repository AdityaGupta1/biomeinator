// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#include "sky/sky_lighting.hlsli"
#include "sky/cloud_traversal.hlsli"
#include "light/fog_density.hlsli"

float cloudOpticalDepth(float3 origin_WS, float3 dir, float maxDistance, inout RandomNumberGenerator rng)
{
    CloudTraversal state = beginCloudTraversal(origin_WS, dir, maxDistance, renderParams.cloud.marchDistance);
    float depth = 0.f;
    float2 interval;
    [loop] while (nextCloudInterval(state, interval))
    {
        depth += renderParams.cloud.density * (interval.y - interval.x);
        if (depth > 12.f) break;
    }
    return depth;
}

float cloudTransmittance(float3 origin_WS, float3 dir, float maxDistance, inout RandomNumberGenerator rng)
{
    return exp(-cloudOpticalDepth(origin_WS, dir, maxDistance, rng));
}

float cloudPhase(float mu, float g)
{
    const float d = max(0.001f, 1.f + g * g - 2.f * g * mu);
    return (1.f - g * g) / (4.f * M_PI * d * sqrt(d));
}

float3 cloudUnshadowedColor(float3 dir)
{
    const CloudSettings c = renderParams.cloud;
    const float3 sunDir = getSunDir_WS();
    const float phase = cloudPhase(dot(dir, sunDir), c.phaseG)
        + c.multiScatterStrength * 0.65f / (4.f * M_PI);
    return c.ambient * getSkyColor(float3(0.f, 1.f, 0.f))
        + phase * getVolumeSunEnergy(sunDir, c.baseHeight + 0.5f * c.thickness);
}

float3 cloudGuideColor(float3 dir, float3 skyColor, float transmittance)
{
    if (transmittance == 1.f) return skyColor;
    return lerp(cloudUnshadowedColor(dir), skyColor, transmittance);
}

struct CloudResult
{
    float3 radiance;
    float transmittance;
};

CloudResult integrateClouds(float3 origin_WS, float3 dir, CloudTraversal state,
    bool cloudHit, float2 interval, float fogDistance, float waterDistance,
    bool scatter, inout RandomNumberGenerator rng)
{
    CloudResult result;
    result.radiance = 0.f;
    result.transmittance = 1.f;
    if (!cloudHit) return result;
    const CloudSettings c = renderParams.cloud;
    const float3 ambient = c.ambient * getSkyColor(float3(0.f, 1.f, 0.f));
    [loop] do
    {
        const float intervalT = exp(-c.density * (interval.y - interval.x));
        if (scatter)
        {
            float3 lighting = 0.f;
            [loop] for (uint i = 0; i < c.samples; ++i)
            {
                // Sample the extinction-weighted interval, including its visible skin when opaque.
                const float u = (float(i) + rng.nextFloat()) / float(c.samples);
                const float distance = interval.x - log(max(1.e-7f, 1.f - u * (1.f - intervalT))) / c.density;
                const float3 pos_WS = origin_WS + dir * distance;
                const float3 sunDir = sampleSunDirection(getSunDir_WS(), rng);
                const float worldY = pos_WS.y + float(cameraParams.globalInstanceOffset.y);
                const float3 sunEnergy = getVolumeSunEnergy(sunDir, worldY);
                float3 source = ambient;
                if (any(sunEnergy > 0.f))
                {
                    const float depth = cloudOpticalDepth(pos_WS, sunDir, 1.e30f, rng);
                    float sunlight = cloudPhase(dot(dir, sunDir), c.phaseG) * exp(-depth);
                    sunlight += c.multiScatterStrength
                        * (0.45f * exp(-0.35f * depth) + 0.2f * exp(-0.12f * depth)) / (4.f * M_PI);
                    source += sunEnergy * sunlight;
                }
                const float fogT = fogDistance > 0.f
                    ? computeFogTransmittance(origin_WS, dir, min(distance, fogDistance)) : 1.f;
                const float3 waterT = computeWaterAbsorption(min(distance, waterDistance));
                lighting += source * fogT * waterT;
            }
            result.radiance += result.transmittance * (1.f - intervalT) * lighting / float(c.samples);
        }
        result.transmittance *= intervalT;
        if (result.transmittance < 0.002f) break;
    } while (nextCloudInterval(state, interval));
    return result;
}
