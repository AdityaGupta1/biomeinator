/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

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

bool isInSun(float3 rayDirection)
{
    const float3 sunDirection = normalize(float3(2.f, 3.f, 4.f));
    return dot(rayDirection, sunDirection) > 0.998f;
}

float3 getDomeLightColor(float3 rayDirection)
{
    if (sceneParams.voxelMode == 0)
    {
        return float3(0.f, 0.f, 0.f);
    }

    if (isInSun(rayDirection))
    {
        return float3(1.f, 0.95f, 0.8f) * 500.f;
    }

    return float3(0.3f, 0.7f, 0.95f);
}

float domeLightPdf(float3 rayDirection)
{
    return 0.f;
}

struct DomeLightSample
{
    bool didReachDomeLight;
    float3 wi_WS;
    float3 Le;
    float pdf;
};

DomeLightSample sampleDomeLight(const float3 surfPos_WS, const float3 surfNor_WS, inout RandomSampler rng)
{
    float3 wi_WS;
    // TODO: generate wi_WS

    RayDesc ray;
    ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
    ray.Direction = wi_WS;
    ray.TMin = 0.f;
    ray.TMax = RAY_DEFAULT_TMAX;

    Payload domeLightPayload;
    domeLightPayload.flags = 0;
    domeLightPayload.pathWeight = float3(1.f, 1.f, 1.f);
    domeLightPayload.pathColor = float3(0.f, 0.f, 0.f);

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_DOME_LIGHT, 0, 0, ray, domeLightPayload);

    DomeLightSample result;

    if (domeLightPayload.flags & PAYLOAD_FLAG_DID_HIT)
    {
        result.didReachDomeLight = false;
    }
    else
    {
        result.didReachDomeLight = true;
        result.wi_WS = wi_WS;
        result.Le = domeLightPayload.pathColor;
        result.pdf = domeLightPdf(wi_WS);
    }

    return result;
}

[shader("closesthit")]
void ClosestHit_DomeLight(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}
