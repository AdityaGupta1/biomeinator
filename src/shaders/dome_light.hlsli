/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "../rendering/common/common_hitgroups.h"

#include "global_params.hlsli"
#include "path_tracing_common.hlsli"
#include "util/rng.hlsli"
#include "util/sampling.hlsli"

static const float3 sunDir_WS = normalize(float3(1.f, 2.f, 4.f));
static const float sunCosTheta = 0.9985f;

static const float3 skyColor = float3(0.3f, 0.7f, 0.95f);
static const float3 sunColor = float3(1.f, 0.95f, 0.8f) * 800.f;

bool isInSun(float3 wi_WS)
{
    return dot(wi_WS, sunDir_WS) >= sunCosTheta;
}

float3 getDomeLightColor(float3 wi_WS)
{
    if (sceneParams.voxelMode == 0)
    {
        return float3(0.f, 0.f, 0.f);
    }

    if (isInSun(wi_WS))
    {
        return sunColor;
    }

    return skyColor;
}

float domeLightPdf(float3 wi_WS, float3 surfNor_WS)
{
    if (sceneParams.voxelMode == 0)
    {
        return 0.f;
    }

    if (isInSun(wi_WS))
    {
        return sphericalCapUniformPdf(wi_WS, sunDir_WS, sunCosTheta);
    }
    else
    {
        return 0.f;
    }
}

struct DomeLightSample
{
    bool didReachDomeLight;
    float3 wi_WS;
    float3 Le;
    float pdf;
};

float3 generateDomeLightSampleDir(const float3 surfNor_WS, inout RandomNumberGenerator rng, out float pdf)
{
    const float3 wi_WS = sampleSphericalCapUniform(sunDir_WS, sunCosTheta, rng);
    pdf = sphericalCapUniformPdf(wi_WS, sunDir_WS, sunCosTheta);
    return wi_WS;
}

DomeLightSample sampleDomeLight(const float3 surfPos_WS, const float3 surfNor_WS, const bool canPassthrough, inout RandomNumberGenerator rng)
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
    domeLightPayload.flags = canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0;
    domeLightPayload.pathWeight = float3(1.f, 1.f, 1.f);
    domeLightPayload.pathColor = float3(0.f, 0.f, 0.f);

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_DOME_LIGHT, 0, 0, ray, domeLightPayload);

    result.didReachDomeLight = !bool(domeLightPayload.flags & PAYLOAD_FLAG_DID_HIT);
    result.wi_WS = wi_WS;
    result.Le = result.didReachDomeLight ? getDomeLightColor(ray.Direction) * domeLightPayload.pathWeight : float3(0.f, 0.f, 0.f);
    result.pdf = pdf;
    return result;
}

[shader("closesthit")]
void ClosestHit_DomeLight(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}
