// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// NOTE: needs the sun constants, so this file must be included after dome_light.hlsli
// (volume.hlsli holds the sun-independent fog math and is included much earlier).

#include "../rendering/common/common_hitgroups.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "light/dome_light.hlsli"
#include "util/math.hlsli"

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
