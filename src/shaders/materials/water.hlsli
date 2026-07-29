// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// NOTE: this file is intended to be included from path_tracing_common.hlsli.

#include "common/global_params.hlsli"
#include "common/payload.hlsli"

static const float3 waterSigmaA = float3(0.35f, 0.06f, 0.02f) * 0.4f;

float3 computeWaterAbsorption(const float dist)
{
    return exp(-waterSigmaA * dist);
}

float getDistanceToVoxelBounds(const float3 origin, const float3 dir)
{
    const float3 boundsMin = float3(sceneParams.voxelBoundsMin_WS);
    const float3 boundsMax = float3(sceneParams.voxelBoundsMax_WS);

    const float3 invDir = rcp(dir);
    const float3 t0 = (boundsMin - origin) * invDir;
    const float3 t1 = (boundsMax - origin) * invDir;

    const float3 tFar = max(t0, t1);
    float tExit = min(tFar.x, min(tFar.y, tFar.z));

    // Handle Y-axis: ray may start outside bounds
    const float3 tNear = min(t0, t1);
    float tEnter = max(tNear.y, 0.f);

    return (tExit > tEnter) ? tExit : 0.f;
}

// Length of a path segment through in-bounds media: distance to the hit, or to the voxel
// bounds exit on a miss.
float getSegmentVolumeDistance(const Payload payload, const float3 rayOrigin, const float3 rayDir)
{
    return bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? distance(rayOrigin, payload.hitInfo.hitPos_WS)
        : getDistanceToVoxelBounds(rayOrigin, rayDir);
}

void setUnderwaterFromHit(inout Payload payload, const bool wasBackfaceHit)
{
    if (wasBackfaceHit)
    {
        payload.flags &= ~PAYLOAD_FLAG_UNDERWATER;
    }
    else
    {
        payload.flags |= PAYLOAD_FLAG_UNDERWATER;
    }
}

float3 computeSegmentAbsorption(const Payload payload, const float3 rayOrigin, const float3 rayDir)
{
    if (!bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        return float3(1.f, 1.f, 1.f);
    }

    return computeWaterAbsorption(getSegmentVolumeDistance(payload, rayOrigin, rayDir));
}

float3 computePassthroughAbsorption(const Payload payload, const float rayEndT)
{
    // waterEntryT is initialized to 0 if the ray starts underwater, RAY_DEFAULT_TMAX otherwise.
    // waterExitT is initialized to RAY_DEFAULT_TMAX and updated in AnyHit on water surface hits.
    // NOTE: this correctly handles one water entry and exit (in that order) along the ray. It breaks
    // down if there are multiple distinct water bodies along the ray (e.g. two separate ponds).
    const float waterLength = max(min(payload.waterExitT, rayEndT) - payload.waterEntryT, 0.f);
    return computeWaterAbsorption(waterLength);
}
