// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "common/global_params.hlsli"
#include "common/payload.hlsli"
#include "materials/water.hlsli"
#include "sky/sky_lighting.hlsli"
#include "util/math.hlsli"

// Aerial haze fades distant terrain toward a desaturated sky color, so the far LODs read as silhouettes.
// Unlike the fog it is not a lit medium: it is applied once per path, over all the air the path crossed
// before its first non-specular hit, so reflections and refractions show the haze of their whole visible
// length. It stops at water: a path entering water is hazed over the distance to the surface.
struct HazeState
{
    float airDistance;
    bool isApplied;
};

HazeState makeHazeState(const bool isEnabled)
{
    HazeState haze;
    haze.airDistance = 0.f;
    haze.isApplied = !isEnabled || renderParams.hazeSigmaS <= 0.f;
    return haze;
}

// The sky straight up rather than toward the horizon or the sun, whose colors are warm and directional
float3 getHazeColor()
{
    const float3 zenithColor = getSkyColor(float3(0.f, 1.f, 0.f));
    return renderParams.hazeBrightness * lerp(zenithColor, luminance(zenithColor).xxx, renderParams.hazeWhiteness);
}

// How much of the haze the sky gets in a direction. The sky's air reaches the top of the world, so at full
// strength the haze would tint most of the sky; it only needs to blend the horizon into the hazed terrain.
float getSkyHazeStrength(const float3 dir)
{
    return 1.f - smoothstep(0.f, renderParams.hazeSkyBand, dir.y);
}

// Dims what lies behind by the haze over the air distance so far, scaled by strength, and adds the haze
// itself, weighted by inScatterWeight: the path weight in front of the surface that ended the air path
void applyHaze(inout HazeState haze, inout float3 pathWeight, const float3 inScatterWeight, inout float3 pathColor,
    const float strength = 1.f)
{
    if (haze.isApplied)
    {
        return;
    }
    haze.isApplied = true;
    const float transmittance = lerp(1.f,
        exp(-renderParams.hazeSigmaS * max(haze.airDistance - renderParams.hazeStartDistance, 0.f)), strength);
    pathColor += inScatterWeight * (1.f - transmittance) * getHazeColor();
    pathWeight *= transmittance;
}

// The segment just traced, if it crossed air
void accumulateHazeDistance(inout HazeState haze, const Payload payload, const float3 origin_WS, const float3 dir)
{
    if (!haze.isApplied && !bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        haze.airDistance += getSegmentVolumeDistance(payload, origin_WS, dir);
    }
}

void applyHazeIfUnderwater(inout HazeState haze, inout Payload payload, const float3 inScatterWeight, inout float3 pathColor)
{
    if (bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        applyHaze(haze, payload.pathWeight, inScatterWeight, pathColor);
    }
}
