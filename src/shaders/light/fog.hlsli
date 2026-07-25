// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// NOTE: needs the sun constants, so this file must be included after dome_light.hlsli.

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_settings.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "light/dome_light.hlsli"
#include "util/math.hlsli"

static const float fogSeaLevelY = float(SEA_LEVEL);
static const float fogRampBlocks = 24.f;

// Fog density profile in true world-space Y: zero below (seaLevel - fogRampBlocks), linear
// ramp up to seaLevel, exponential falloff above. Linear (not smoothstep) in the ramp so the
// optical-depth integral stays closed-form; the ramp is mostly underground anyway.
float getFogDensity(const float y)
{
    const float rampBottomY = fogSeaLevelY - fogRampBlocks;
    if (y <= rampBottomY)
    {
        return 0.f;
    }
    if (y <= fogSeaLevelY)
    {
        return renderParams.fogSigmaS * (y - rampBottomY) / fogRampBlocks;
    }
    return renderParams.fogSigmaS * exp(-(y - fogSeaLevelY) / renderParams.fogScaleHeight);
}

// Closed-form optical depth of a segment through the fog profile, split at the two zone
// boundary heights. origin_WS is shader world space; true world Y adds globalInstanceOffset.
float computeFogOpticalDepth(const float3 origin_WS, const float3 dir, const float dist)
{
    const float sigmaS = renderParams.fogSigmaS;
    const float scaleHeight = renderParams.fogScaleHeight;
    const float rampBottomY = fogSeaLevelY - fogRampBlocks;

    const float y0 = origin_WS.y + float(cameraParams.globalInstanceOffset.y);
    const float dy = dir.y;

    if (abs(dy) < 1e-4f) // near-horizontal: constant-height limit
    {
        return getFogDensity(y0) * dist;
    }

    const float invDy = rcp(dy);
    const float tAtRampBottom = (rampBottomY - y0) * invDy;
    const float tAtSeaLevel = (fogSeaLevelY - y0) * invDy;

    float opticalDepth = 0.f;

    // ramp zone: density is linear in Y, so the integral is average density times sub-length
    const float rampT0 = clamp(min(tAtRampBottom, tAtSeaLevel), 0.f, dist);
    const float rampT1 = clamp(max(tAtRampBottom, tAtSeaLevel), 0.f, dist);
    if (rampT1 > rampT0)
    {
        const float yMid = y0 + dy * 0.5f * (rampT0 + rampT1);
        opticalDepth += sigmaS * ((yMid - rampBottomY) / fogRampBlocks) * (rampT1 - rampT0);
    }

    // exponential zone
    const float expT0 = (dy > 0.f) ? clamp(tAtSeaLevel, 0.f, dist) : 0.f;
    const float expT1 = (dy > 0.f) ? dist : clamp(tAtSeaLevel, 0.f, dist);
    if (expT1 > expT0)
    {
        const float yA = y0 + dy * expT0;
        const float yB = y0 + dy * expT1;
        opticalDepth += sigmaS * scaleHeight * invDy *
            (exp(-(yA - fogSeaLevelY) / scaleHeight) - exp(-(yB - fogSeaLevelY) / scaleHeight));
    }

    return opticalDepth;
}

float computeFogTransmittance(const float3 origin_WS, const float3 dir, const float dist)
{
    return exp(-computeFogOpticalDepth(origin_WS, dir, dist));
}

// Optical depth from a point (true world Y) out of the atmosphere along an upward direction
// (dirY > 0); the exponential zone integrates to a finite value out to infinity.
float computeFogOpticalDepthToSky(const float y, const float dirY)
{
    const float sigmaS = renderParams.fogSigmaS;
    const float scaleHeight = renderParams.fogScaleHeight;
    const float rampBottomY = fogSeaLevelY - fogRampBlocks;
    const float invDy = rcp(max(dirY, 1e-3f));

    float opticalDepth = 0.f;
    if (y < fogSeaLevelY)
    {
        const float yA = max(y, rampBottomY);
        opticalDepth += sigmaS * ((0.5f * (yA + fogSeaLevelY) - rampBottomY) / fogRampBlocks) * (fogSeaLevelY - yA) * invDy;
    }

    const float yExp = max(y, fogSeaLevelY);
    opticalDepth += sigmaS * scaleHeight * invDy * exp(-(yExp - fogSeaLevelY) / scaleHeight);

    return opticalDepth;
}

float henyeyGreensteinPhase(const float cosAngle, const float g)
{
    const float g2 = g * g;
    const float denom = 1.f + g2 - 2.f * g * cosAngle;
    return (1.f - g2) / (4.f * M_PI * denom * sqrt(denom));
}

bool isRayOccluded(const float3 pos_WS, const float3 dir)
{
    RayDesc ray;
    ray.Origin = pos_WS;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = RAY_DEFAULT_TMAX;

    // Only the miss shader can run (FORCE_OPAQUE + SKIP_CLOSEST_HIT_SHADER), and it clears
    // PAYLOAD_FLAG_DID_HIT, so seed the flag and check whether it survived. FORCE_OPAQUE
    // means alpha-cutout foliage and water surfaces occlude fully; acceptable for now.
    Payload payload = (Payload)0;
    payload.flags = PAYLOAD_FLAG_DID_HIT;
    TraceRay(raytracingAcs,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);
    return bool(payload.flags & PAYLOAD_FLAG_DID_HIT);
}

// Marches the fog along a segment, accumulating single-scattered sunlight plus a cheap
// analytic sky ambient term (aerial perspective). Returns radiance to be multiplied by the
// path weight at the segment start and outputs the segment's fog transmittance, which the
// caller applies to pathWeight separately. numSteps == 0 skips the march and the ambient
// term, returning zero radiance (but still a valid transmittance).
float3 computeFogInScatter(const float3 origin_WS, const float3 dir, const float dist, const uint numSteps,
    inout RandomNumberGenerator rng, out float segmentTransmittance)
{
    segmentTransmittance = computeFogTransmittance(origin_WS, dir, dist);
    if (numSteps == 0)
    {
        return float3(0.f, 0.f, 0.f);
    }

    const float globalOffsetY = float(cameraParams.globalInstanceOffset.y);
    const float3 sunDir_WS = getSunDir_WS();

    float3 inScatter = float3(0.f, 0.f, 0.f);

    // Below the horizon the sun contributes nothing, and computeFogOpticalDepthToSky's
    // parameterization assumes an upward direction, so skip the march entirely at night.
    if (!isSunOccluded(sunDir_WS))
    {
        const float phase = henyeyGreensteinPhase(dot(dir, sunDir_WS), renderParams.fogG);

        const float stepLength = dist / numSteps;
        float sunScatter = 0.f;
        for (uint stepIdx = 0; stepIdx < numSteps; ++stepIdx)
        {
            const float t = (stepIdx + rng.nextFloat()) * stepLength;
            const float3 stepPos_WS = origin_WS + dir * t;
            const float density = getFogDensity(stepPos_WS.y + globalOffsetY);
            if (density <= 0.f)
            {
                continue;
            }

            if (isRayOccluded(stepPos_WS, sunDir_WS))
            {
                continue;
            }

            const float viewTransmittance = computeFogTransmittance(origin_WS, dir, t);
            const float sunTransmittance = exp(-computeFogOpticalDepthToSky(stepPos_WS.y + globalOffsetY, sunDir_WS.y));
            sunScatter += viewTransmittance * density * sunTransmittance * stepLength;
        }

        // getSunColor is radiance over the sun disk, so undo the solid-angle division to get
        // illuminance; this also picks up atmospheric transmittance, reddening rays at sunset.
        inScatter = sunScatter * phase * sunSolidAngle * getSunColor(sunDir_WS);
    }

    // NOTE: no visibility check, so this also brightens enclosed spaces (cave interiors)
    // with sky-colored haze; fogAmbientStrength is the artistic control for how much.
    inScatter += renderParams.fogAmbientStrength * (1.f - segmentTransmittance) * getSkyColor(float3(0.f, 1.f, 0.f));

    return inScatter;
}
