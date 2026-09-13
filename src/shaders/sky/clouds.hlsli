// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#include "sky/sky_lighting.hlsli"
#include "sky/cloud_density.hlsli"
#include "light/fog_density.hlsli"

// 60-degree vertical FOV / 720 pixels, over about 5 km to clouds plus a 200-block reflection.
// Fixed in world units; the live ray cone still carries the actual camera and scattering footprint.
static const float cloudFineFootprint = 8.f;

bool cloudInterval(float3 origin, float3 dir, float maxDistance, out float start, out float end)
{
    start = 0.f;
    end = maxDistance;
    const CloudSettings c = renderParams.cloud;
    if (sceneParams.voxelMode == 0 || renderParams.clouds == 0 || c.coverage <= 0.f || c.density <= 0.f)
        return false;
    if (abs(dir.y) < 1.e-7f)
    {
        if (origin.y < c.baseHeight || origin.y > c.baseHeight + c.thickness)
            return false;
    }
    else
    {
        const float a = (c.baseHeight - origin.y) / dir.y;
        const float b = (c.baseHeight + c.thickness - origin.y) / dir.y;
        start = max(0.f, min(a, b));
        end = min(end, max(a, b));
    }
    end = min(end, start + c.marchDistance);
    return end > start;
}

float cloudOpticalDepth(float3 origin_WS, float3 dir, float maxDistance, inout RandomNumberGenerator rng)
{
    const float3 origin = cloudPosition(origin_WS);
    float start, end;
    if (!cloudInterval(origin, dir, maxDistance, start, end))
        return 0.f;
    const float ds = renderParams.cloud.lightStepSize;
    float depth = 0.f;
    [loop] for (float t = start; t < end; t += ds)
    {
        const float length = min(ds, end - t);
        depth += cloudDensity(origin + dir * (t + rng.nextFloat() * length), false) * length;
        if (depth > 8.f)
            break;
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

struct CloudResult
{
    float3 radiance;
    float transmittance;
};

CloudResult integrateClouds(float3 origin_WS, float3 dir, float maxDistance, RayCone cone,
    float fogDistance, float waterDistance, bool scatter, inout RandomNumberGenerator rng)
{
    CloudResult result;
    result.radiance = 0.f;
    result.transmittance = 1.f;
    const float3 origin = cloudPosition(origin_WS);
    float start, end;
    if (!cloudInterval(origin, dir, min(maxDistance, renderParams.cloud.maxDistance), start, end))
        return result;
    const CloudSettings c = renderParams.cloud;
    const float3 ambient = c.ambient * getSkyColor(float3(0.f, 1.f, 0.f));
    [loop] for (float t = start; t < end;)
    {
        const bool fine = getRayConeWidthAtDistance(cone, t) < cloudFineFootprint;
        const float ds = min(fine ? c.stepSize : c.secondaryStepSize, end - t);
        const float distance = t + rng.nextFloat() * ds;
        const float3 pos_WS = origin_WS + dir * distance;
        const float extinction = cloudDensity(origin + dir * distance,
            getRayConeWidthAtDistance(cone, distance) < cloudFineFootprint);
        const float stepT = exp(-extinction * ds);
        if (extinction > 0.f && scatter)
        {
            const float3 sunDir = sampleSunDirection(getSunDir_WS(), rng);
            const float worldY = pos_WS.y + float(cameraParams.globalInstanceOffset.y);
            const float3 sunEnergy = getVolumeSunEnergy(sunDir, worldY);
            float3 source = ambient;
            if (any(sunEnergy > 0.f))
            {
                // Shadow rays always use the base field, including those from detailed view samples.
                const float depth = cloudOpticalDepth(pos_WS, sunDir, 1.e30f, rng);
                float sunlight = cloudPhase(dot(dir, sunDir), c.phaseG) * exp(-depth);
                if (c.multiScatter != 0.f)
                    sunlight += (0.45f * exp(-0.35f * depth) + 0.2f * exp(-0.12f * depth)) / (4.f * M_PI);
                source += sunEnergy * sunlight;
            }
            const float fogT = fogDistance > 0.f
                ? computeFogTransmittance(origin_WS, dir, min(distance, fogDistance)) : 1.f;
            const float3 waterT = computeWaterAbsorption(min(distance, waterDistance));
            result.radiance += result.transmittance * (1.f - stepT) * source * fogT * waterT;
        }
        result.transmittance *= stepT;
        if (result.transmittance < 0.002f)
            break;
        t += ds;
    }
    return result;
}
