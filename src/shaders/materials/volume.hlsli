// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// NOTE: this file is intended to be included from path_tracing_common.hlsli, after instanceDatas
// and perTriDatas are declared.

#include "../rendering/common/common_settings.h"

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

    const float dist = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? distance(rayOrigin, payload.hitInfo.hitPos_WS)
        : getDistanceToVoxelBounds(rayOrigin, rayDir);
    return computeWaterAbsorption(dist);
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
