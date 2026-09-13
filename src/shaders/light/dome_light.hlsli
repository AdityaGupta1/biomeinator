// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "sky/atmosphere.hlsli"
#include "util/rng.hlsli"
#include "util/sampling.hlsli"

#include "sky/sky_lighting.hlsli"
#include "sky/clouds.hlsli"

float3 getDomeLightColor(float3 wi_WS)
{
    if (sceneParams.voxelMode == 0)
    {
        return float3(0.f, 0.f, 0.f);
    }

    if (isInSun(wi_WS) && !isSunOccluded(wi_WS))
    {
        return getSunColor(wi_WS);
    }

    return getSkyColor(wi_WS);
}

float domeLightPdf(float3 wi_WS, float3 surfShadingNor_WS)
{
    if (sceneParams.voxelMode == 0)
    {
        return 0.f;
    }

    if (isInSun(wi_WS))
    {
        return sphericalCapUniformPdf(wi_WS, getSunDir_WS(), sunCosTheta);
    }

    return 0.f;
}

struct DomeLightSample
{
    bool didReachDomeLight;
    float3 wi_WS;
    float3 Le;
    float pdf;
};

// TODO: Once the moon exists, NEE should sample its cap as well, based on whether the sun is up at the time. Also,
// domeLightPdf must account for both caps to keep MIS consistent.
float3 generateDomeLightSampleDir(const float3 surfShadingNor_WS, inout RandomNumberGenerator rng, out float pdf)
{
    const float3 sunDir_WS = getSunDir_WS();
    const float3 wi_WS = sampleSphericalCapUniform(sunDir_WS, sunCosTheta, rng);
    // The sample is inside the cap by construction; testing the rounded direction against the cap
    // edge would give boundary samples a zero pdf due to float precision
    pdf = sphericalCapUniformPdfInside(sunCosTheta);
    return wi_WS;
}

DomeLightSample sampleDomeLight(const float3 surfPos_WS,
                                const float3 surfShadingNor_WS,
                                const float3 surfGeoNor_WS,
                                const RayCone rayCone,
                                const bool canPassthrough,
                                const bool startUnderwater,
                                const bool acceptsBacksideLight,
                                inout RandomNumberGenerator rng)
{
    DomeLightSample result;

    float3 wi_WS;
    float pdf;
    wi_WS = generateDomeLightSampleDir(surfShadingNor_WS, rng, pdf);

    // Opaque surfaces require light above both the shading and geometric horizons.
    // Transmission surfaces accept backside light. Rejecting opaque backside samples here saves a
    // shadow ray, including when the sun is below the shading point's horizon.
    if (!acceptsBacksideLight &&
        (dot(wi_WS, surfShadingNor_WS) <= 0.f || dot(wi_WS, surfGeoNor_WS) <= 0.f))
    {
        result.didReachDomeLight = false;
        return result;
    }

    RayDesc ray;
    setRayOriginAndDirection(ray, surfPos_WS, surfGeoNor_WS, wi_WS, true /*faceforwardNormal*/);
    ray.TMin = 0.f;
    ray.TMax = RAY_DEFAULT_TMAX;

    // Occlusion-only ray: the candidate handling still runs on non-opaque geometry, preserving
    // passthrough tint and water entry/exit tracking for absorption.
    Payload domeLightPayload;
    domeLightPayload.flags =
        (canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0) |
        (startUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0);
    domeLightPayload.pathWeight = float3(1.f, 1.f, 1.f);
    domeLightPayload.rng = rng;
    domeLightPayload.waterEntryT = startUnderwater ? 0.f : RAY_DEFAULT_TMAX;
    domeLightPayload.waterExitT = RAY_DEFAULT_TMAX;
    domeLightPayload.rayCone = rayCone;

    result.didReachDomeLight = !isSegmentOccluded(ray, domeLightPayload);
    result.wi_WS = wi_WS;
    result.pdf = pdf;
    if (result.didReachDomeLight)
    {
        const float3 passthroughAbsorption = computePassthroughAbsorption(domeLightPayload, getDistanceToVoxelBounds(ray.Origin, ray.Direction));
        const float3 sunEnergy = getVolumeSunEnergy(ray.Direction,
            ray.Origin.y + float(cameraParams.globalInstanceOffset.y));
        result.Le = sunEnergy / sunSolidAngle * domeLightPayload.pathWeight * passthroughAbsorption;
        if (any(result.Le > 0.f))
            result.Le *= cloudTransmittance(ray.Origin, ray.Direction, 1.e30f, rng);
    }
    else
    {
        result.Le = float3(0.f, 0.f, 0.f);
    }
    return result;
}
