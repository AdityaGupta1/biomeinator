// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "sky/cloud_model.hlsli"

SamplerState cloudSampler : REGISTER_S(RT, CLOUD_SAMPLER);
SamplerState cloudFieldSampler : REGISTER_S(RT, CLOUD_FIELD_SAMPLER);

float3 cloudAdvectedPosition(float3 worldPos)
{
    worldPos.xz -= cloudWind * renderParams.animTime;
    return worldPos;
}

float cloudExtinction(float3 p, float body)
{
    return cloudDetailDensity(body, p) * renderParams.cloudDensity;
}

bool cloudInterval(float3 worldOrigin, float3 dir, out float start, out float end)
{
    start = 0.f;
    end = 0.f;
    if (renderParams.clouds == 0 || sceneParams.voxelMode == 0 || renderParams.cloudDensity <= 0.f || renderParams.cloudCoverage <= 0.f)
        return false;
    if (abs(dir.y) < 1.e-6f)
    {
        end = cloudMaxDistance;
        return worldOrigin.y > cloudBase && worldOrigin.y < cloudTop;
    }
    const float a = (cloudBase - worldOrigin.y) / dir.y;
    const float b = (cloudTop - worldOrigin.y) / dir.y;
    start = max(0.f, min(a, b));
    end = min(cloudMaxDistance, max(a, b));
    return end > start;
}

// Cached optical depth in world space, not a directional environment map.
float cloudLightDepth(float3 advectedPos)
{
    Texture3D<float> light = ResourceDescriptorHeap[heapIndices.srv.cloudLightIdx];
    return light.SampleLevel(cloudFieldSampler, cloudFieldUv(advectedPos), 0);
}

float cloudSunTransmittance(float3 origin_WS, float3 dir)
{
    const float3 origin = origin_WS + float3(cameraParams.globalInstanceOffset);
    float start, end;
    if (!cloudInterval(origin, dir, start, end))
        return 1.f;
    return exp(-cloudLightDepth(cloudAdvectedPosition(origin + dir * start)));
}

float cloudPhase(float mu, float g)
{
    return (1.f - g * g) / (4.f * M_PI * pow(max(0.001f, 1.f + g * g - 2.f * g * mu), 1.5f));
}

float4 integrateClouds(float3 origin_WS, float3 dir, uint steps)
{
    const float3 origin = origin_WS + float3(cameraParams.globalInstanceOffset);
    float start, end;
    if (!cloudInterval(origin, dir, start, end))
        return float4(0.f, 0.f, 0.f, 1.f);

    const float3 sunDir = getSunDir_WS();
    const float3 sunEnergy = getVolumeSunEnergy(sunDir, cloudBase + 0.5f * CLOUD_CONFIG.thickness);
    const float phase = 0.85f * cloudPhase(dot(dir, sunDir), renderParams.cloud.phaseG) + 0.15f * cloudPhase(dot(dir, sunDir), -0.25f);
    const float3 ambient = getSkyColor(float3(0.f, 1.f, 0.f)) * renderParams.cloud.ambient;
    const float3 clearSky = getSkyColor(dir);
    Texture3D<float> shape = ResourceDescriptorHeap[heapIndices.srv.cloudShapeIdx];
    // Draw distance limits entry distance; the integration budget limits travel
    // INSIDE the layer. Never dilute 128 samples across a 100 km horizontal ray.
    end = min(end, start + min(12000.f, 6.f * CLOUD_CONFIG.thickness));
    steps = max(steps, 1u);
    const float stepLength = (end - start) / steps;
    // Stratify the march instead of exposing parallel sample planes as bands.
    // Keep the offset stable across frames; cloud sampling must not sparkle
    // independently of path sampling in an otherwise stationary view.
    uint cloudSeed = hash(asuint(dir.x) ^ hash(asuint(dir.y)) ^ hash(asuint(dir.z)));
    const float jitter = float(cloudSeed & 0x00ffffffu) / 16777216.f;
    float transmittance = 1.f;
    float3 radiance = 0.f;
    [loop] for (uint i = 0; i < steps; ++i)
    {
        const float distance = start + (i + jitter) * stepLength;
        const float3 pos = cloudAdvectedPosition(origin + dir * distance);
        const float body = cloudBodyMargin(pos, shape, cloudFieldSampler, renderParams.cloudCoverage);
        if (body <= 0.f)
            continue;
        const float extinction = cloudExtinction(pos, body);
        if (extinction <= 0.f)
            continue;
        const float lightDepth = cloudLightDepth(pos);
        // Analytic integration per step with a scattering-octave approximation for
        // bright cloud interiors. This is not a multiple-scattering path tracer.
        const float scatter = phase * exp(-lightDepth) +
            renderParams.cloud.multiScatter * (0.45f * exp(-lightDepth * 0.35f) + 0.2f * exp(-lightDepth * 0.12f)) / (4.f * M_PI);
        const float height = saturate((pos.y - cloudBase) / (cloudTop - cloudBase));
        const float powder = 1.f - exp(-extinction * 120.f);
        float3 source = ambient * lerp(0.45f, 1.f, height) + sunEnergy * scatter * lerp(1.f, lerp(0.65f, 1.35f, powder), renderParams.cloud.powder);
        // Distant clouds approach the atmospheric sky color instead of cutting a
        // hard white band into the horizon. Approximate aerial perspective only.
        source = lerp(clearSky, source, exp(-distance * renderParams.cloud.aerial));
        const float stepT = exp(-extinction * stepLength);
        radiance += transmittance * (1.f - stepT) * source;
        transmittance *= stepT;
        if (transmittance < 0.002f)
            break;
    }
    return float4(radiance, transmittance);
}

float3 compositeClouds(float3 origin_WS, float3 dir, float3 background, uint steps)
{
    const float4 cloud = integrateClouds(origin_WS, dir, steps);
    return cloud.rgb + cloud.a * background;
}
