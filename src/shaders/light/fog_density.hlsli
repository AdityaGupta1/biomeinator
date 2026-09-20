// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta
#pragma once
#include "../rendering/common/common_settings.h"
#include "common/global_params.hlsli"

static const float fogSeaLevelY = float(SEA_LEVEL);
static const float fogUndergroundRampBlocks = 24.f;

// Fog density profile in true world-space Y: zero below (seaLevel - fogUndergroundRampBlocks), linear ramp up to
// seaLevel, exponential falloff above. Linear (not smoothstep) in the ramp so the optical-depth integral stays
// closed-form; the ramp is mostly underground anyway.
float getFogDensity(const float y)
{
    const float rampBottomY = fogSeaLevelY - fogUndergroundRampBlocks;
    if (y <= rampBottomY)
    {
        return 0.f;
    }
    if (y <= fogSeaLevelY)
    {
        return renderParams.fogSigmaS * (y - rampBottomY) / fogUndergroundRampBlocks;
    }
    return renderParams.fogSigmaS * exp(-(y - fogSeaLevelY) / renderParams.fogScaleHeight);
}

// Closed-form optical depth of a segment through the fog profile, split at the two zone
// boundary heights. origin_WS is shader world space; true world Y adds globalInstanceOffset.
float computeFogOpticalDepth(const float3 origin_WS, const float3 dir, const float dist)
{
    const float sigmaS = renderParams.fogSigmaS;
    const float scaleHeight = renderParams.fogScaleHeight;
    const float rampBottomY = fogSeaLevelY - fogUndergroundRampBlocks;

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
        const float yMid = y0 + ((rampT0 + rampT1) * 0.5f) * dy;
        const float densityYMid = sigmaS * ((yMid - rampBottomY) / fogUndergroundRampBlocks);
        opticalDepth += densityYMid * (rampT1 - rampT0);
    }

    // exponential zone
    const float expT0 = (dy > 0.f) ? clamp(tAtSeaLevel, 0.f, dist) : 0.f;
    const float expT1 = (dy > 0.f) ? dist : clamp(tAtSeaLevel, 0.f, dist);
    if (expT1 > expT0)
    {
        const float yA = y0 + dy * expT0;
        const float yB = y0 + dy * expT1;
        const float densityYA = sigmaS * exp(-(yA - fogSeaLevelY) / scaleHeight);
        const float densityYB = sigmaS * exp(-(yB - fogSeaLevelY) / scaleHeight);
        opticalDepth += (densityYA - densityYB) * scaleHeight * invDy;
    }

    return opticalDepth;
}

float computeFogTransmittance(const float3 origin_WS, const float3 dir, const float dist)
{
    return exp(-computeFogOpticalDepth(origin_WS, dir, dist));
}
