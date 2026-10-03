// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// NOTE: this file is intended to be included from path_tracing_common.hlsli.

#include "common/global_params.hlsli"
#include "common/payload.hlsli"
#include "materials/media.hlsli"

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

// The medium on the far side of the hit face, which a transmitted path enters. Thin sheets (foliage, alpha
// passthrough) have the same medium on both sides.
uint getFarSideMedium(const Payload payload, const PerFaceData perFaceData)
{
    if (!perFaceData.isMediumBoundary())
    {
        return getPayloadMedium(payload);
    }
    return bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT) ? perFaceData.getFrontMedium() : perFaceData.getBackMedium();
}

void transmitThroughFace(inout Payload payload, const PerFaceData perFaceData)
{
    setPayloadMedium(payload, getFarSideMedium(payload, perFaceData));
}

// surfMedia holds the medium on the side of the surface the path arrived from (which surfGeoNor_WS faces)
// and on the far side; a shadow ray starts in whichever it leaves into.
bool isShadowRayStartUnderwater(const uint2 surfMedia, const float3 wi_WS, const float3 surfGeoNor_WS)
{
    return ((dot(wi_WS, surfGeoNor_WS) >= 0.f) ? surfMedia.x : surfMedia.y) == MEDIUM_WATER;
}

float3 computeSegmentAbsorption(const Payload payload, const float3 rayOrigin, const float3 rayDir)
{
    const uint medium = getPayloadMedium(payload);
    if (medium == MEDIUM_AIR)
    {
        return float3(1.f, 1.f, 1.f);
    }

    return exp(-mediumSigmaAs[medium] * getSegmentVolumeDistance(payload, rayOrigin, rayDir));
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
