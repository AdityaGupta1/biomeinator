// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#include "sky/sky_lighting.hlsli"
#include "sky/cloud_density.hlsli"
#include "light/fog_density.hlsli"

// Fixed world-space footprint; allow distant cloud detail even at reduced DLSS resolution.
// The live cone still accounts for the camera and widening from scattering.
static const float cloudFineFootprint = 64.f;

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
    return end > start;
}

float cloudOpticalDepth(float3 origin_WS, float3 dir, float maxDistance, inout RandomNumberGenerator rng)
{
    const float3 origin = cloudPosition(origin_WS);
    float start, end;
    if (!cloudInterval(origin, dir, min(maxDistance, renderParams.cloud.maxDistance), start, end))
        return 0.f;
    end = min(end, start + renderParams.cloud.marchDistance);
#if CLOUD_USE_SHAPE_CACHE
    // Low sun rays can enter the layer hundreds of kilometres away. Ignore shadows
    // beyond the local field instead of evaluating uncached procedural noise there.
    const float texel = cloudShapeTexelSize();
    const float2 fieldMin = cloudShapeOrigin() + 0.5f * texel;
    const float2 fieldMax = cloudShapeOrigin() + (float(cloudShapeSize) - 0.5f) * texel;
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        const float o = origin.xz[axis];
        const float d = dir.xz[axis];
        if (abs(d) < 1.e-7f)
        {
            if (o < fieldMin[axis] || o > fieldMax[axis])
                return 0.f;
        }
        else
        {
            const float a = (fieldMin[axis] - o) / d;
            const float b = (fieldMax[axis] - o) / d;
            start = max(start, min(a, b));
            end = min(end, max(a, b));
        }
    }
#endif
    // Grazing visibility only needs the broad field. Spread six samples across long
    // horizontal intervals, fading back to the configured spacing above the horizon.
    const float grazing = saturate(1.f - abs(dir.y) / 0.15f);
    const float ds = max(renderParams.cloud.lightStepSize, (end - start) * grazing / 6.f);
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

float cloudViewStep(float t, float3 dir, RayCone cone)
{
    const CloudSettings c = renderParams.cloud;
    const bool fine = getRayConeWidthAtDistance(cone, t) < cloudFineFootprint;
    const float nearStep = fine ? c.stepSize : c.secondaryStepSize;
    const float featureStep = min(c.period / 256.f, c.thickness / (16.f * max(abs(dir.y), 0.01f)));
    return max(nearStep, min(t * 0.02f, featureStep));
}

float3 cloudUnshadowedColor(float3 dir)
{
    const CloudSettings c = renderParams.cloud;
    const float3 sunDir = getSunDir_WS();
    const float phase = cloudPhase(dot(dir, sunDir), c.phaseG)
        + (c.multiScatter != 0.f ? 0.65f / (4.f * M_PI) : 0.f);
    return c.ambient * getSkyColor(float3(0.f, 1.f, 0.f))
        + phase * getVolumeSunEnergy(sunDir, c.baseHeight + 0.5f * c.thickness);
}

float cloudGuideTransmittance(float3 origin_WS, float3 dir)
{
    const float3 origin = cloudPosition(origin_WS);
    float start, end;
    if (!cloudInterval(origin, dir, renderParams.cloud.maxDistance, start, end))
        return 1.f;
    const CloudSettings c = renderParams.cloud;
    const float featureStep = min(c.period / 256.f, c.thickness / (128.f * max(abs(dir.y), 0.01f)));
    float previous = cloudDensity(origin + dir * start, false);
    float depth = 0.f;
    // The albedo needs a smooth silhouette, not fine erosion or sampled lighting.
    [loop] for (float t = start; t < end;)
    {
        const float ds = min(min(max(c.secondaryStepSize, t * 0.04f), featureStep), end - t);
        const float next = cloudDensity(origin + dir * (t + ds), false);
        depth += 0.5f * (previous + next) * ds;
        previous = next;
        if (depth > 8.f)
            break;
        t += ds;
    }
    return exp(-depth);
}

float3 cloudGuideColor(float3 origin_WS, float3 dir, float3 skyColor)
{
    const float transmittance = cloudGuideTransmittance(origin_WS, dir);
    if (transmittance == 1.f)
        return skyColor;
    return lerp(cloudUnshadowedColor(dir), skyColor, transmittance);
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
        // Grow distant steps, but retain samples across both horizontal lobes and layer height.
        const float ds = min(cloudViewStep(t, dir, cone), end - t);
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
