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

// NOTE: this file is intended to be included from path_tracing_common.hlsli, after instanceDatas
// and perTriDatas are declared.

#include "global_params.hlsli"
#include "payload.hlsli"

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
