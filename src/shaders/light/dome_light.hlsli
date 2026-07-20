// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_hitgroups.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "sky/atmosphere.hlsli"
#include "util/rng.hlsli"
#include "util/sampling.hlsli"

// Deliberately larger than the real sun (~0.8° radius vs. 0.27°)
static const float sunCosTheta = 0.9999f;
static const float sunSolidAngle = M_TWO_PI * (1.f - sunCosTheta);

// Calibrated against the previous hand-tuned sun (radiance 16000 over the oversized disk's solid
// angle, ~10 lux) so overall exposure and tonemapping don't shift drastically.
static const float3 sunIlluminance = float3(10.f, 10.f, 10.f);

// Small constant floor so nights aren't pitch black until the moon exists. Added at the lookup
// rather than baked into the sky-view LUT so it's trivial to delete when the moon lands.
static const float3 nightAmbient = float3(0.01f, 0.015f, 0.025f);

SamplerState skyLutSampler : REGISTER_S(RT, LUT_SAMPLER);
SamplerState skyViewSampler : REGISTER_S(RT, SKY_VIEW_SAMPLER);

float3 getSunDir_WS()
{
    return computeSunDir_WS(renderParams.animTime);
}

bool isInSun(float3 wi_WS)
{
    return dot(wi_WS, getSunDir_WS()) >= sunCosTheta;
}

float getCameraAtmosphereRadius()
{
    return atmosphereRadiusForCameraY(cameraParams.pos_WS.y + cameraParams.globalInstanceOffset.y);
}

float3 getSkyColor(float3 wi_WS)
{
    Texture2D<float4> skyViewLut = ResourceDescriptorHeap[heapIndices.srv.skyViewLutIdx];
    const float2 uv = skyViewDirToUv(wi_WS, getSunDir_WS());
    return skyViewLut.SampleLevel(skyViewSampler, uv, 0).rgb * sunIlluminance + nightAmbient;
}

// True if the ray from the camera towards wi_WS is occluded by the virtual planet. The
// transmittance parameterization only covers rays that don't hit the ground sphere, and isInSun
// alone would show the disk through the horizon at night.
bool isSunOccluded(float3 wi_WS)
{
    const float r = getCameraAtmosphereRadius();
    return raySphereIntersectNearest(float3(0.f, r, 0.f), wi_WS, atmosphereGroundRadius) >= 0.f;
}

float3 getSunColor(float3 wi_WS)
{
    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[heapIndices.srv.transmittanceLutIdx];
    const float3 transmittance = sampleTransmittanceLut(transmittanceLut, skyLutSampler, getCameraAtmosphereRadius(), wi_WS.y);
    return sunIlluminance * transmittance / sunSolidAngle;
}

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

float domeLightPdf(float3 wi_WS, float3 surfNor_WS)
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
float3 generateDomeLightSampleDir(const float3 surfNor_WS, inout RandomNumberGenerator rng, out float pdf)
{
    const float3 sunDir_WS = getSunDir_WS();
    const float3 wi_WS = sampleSphericalCapUniform(sunDir_WS, sunCosTheta, rng);
    pdf = sphericalCapUniformPdf(wi_WS, sunDir_WS, sunCosTheta);
    return wi_WS;
}

DomeLightSample sampleDomeLight(const float3 surfPos_WS,
                                const float3 surfNor_WS,
                                const RayCone rayCone,
                                const bool canPassthrough,
                                const bool startUnderwater,
                                inout RandomNumberGenerator rng)
{
    DomeLightSample result;

    float3 wi_WS;
    float pdf;
    wi_WS = generateDomeLightSampleDir(surfNor_WS, rng, pdf);

    if (dot(wi_WS, surfNor_WS) < 0.f)
    {
        result.didReachDomeLight = false;
        return result;
    }

    RayDesc ray;
    setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, wi_WS, false /*faceforwardNormal*/);
    ray.TMin = 0.f;
    ray.TMax = RAY_DEFAULT_TMAX;

    Payload domeLightPayload;
    domeLightPayload.flags =
        (canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0) |
        (startUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0);
    domeLightPayload.pathWeight = float3(1.f, 1.f, 1.f);
    domeLightPayload.rng = rng;
    domeLightPayload.waterEntryT = startUnderwater ? 0.f : RAY_DEFAULT_TMAX;
    domeLightPayload.waterExitT = RAY_DEFAULT_TMAX;
    domeLightPayload.rayCone = rayCone;

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_DOME_LIGHT, 0, 0, ray, domeLightPayload);

    result.didReachDomeLight = !bool(domeLightPayload.flags & PAYLOAD_FLAG_DID_HIT);
    result.wi_WS = wi_WS;
    result.pdf = pdf;
    if (result.didReachDomeLight)
    {
        const float3 passthroughAbsorption = computePassthroughAbsorption(domeLightPayload, getDistanceToVoxelBounds(ray.Origin, ray.Direction));
        result.Le = getDomeLightColor(ray.Direction) * domeLightPayload.pathWeight * passthroughAbsorption;
    }
    else
    {
        result.Le = float3(0.f, 0.f, 0.f);
    }
    return result;
}

[shader("closesthit")]
void ClosestHit_DomeLight(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}
