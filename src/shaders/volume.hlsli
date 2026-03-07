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

    float tEnter = 0.f;
    float tExit = 1e30f;

    [unroll]
    for (uint axis = 0; axis < 3; ++axis)
    {
        const float o = origin[axis];
        const float d = dir[axis];
        const float bMin = boundsMin[axis];
        const float bMax = boundsMax[axis];

        if (abs(d) < 1e-8f)
        {
            if (o < bMin || o > bMax)
            {
                return 0.f;
            }
            continue;
        }

        const float invDir = 1.f / d;
        float t0 = (bMin - o) * invDir;
        float t1 = (bMax - o) * invDir;
        if (t0 > t1)
        {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }

        tEnter = max(tEnter, t0);
        tExit = min(tExit, t1);
    }

    const float nearT = max(tEnter, 0.f);
    if (tExit <= nearT)
    {
        return 0.f;
    }

    return tExit;
}

bool isWaterTriangle(const InstanceData instanceData, const uint triangleIdx)
{
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + triangleIdx];
    return bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
}

bool isWaterTriangle(const uint instanceId, const uint triangleIdx)
{
    if (sceneParams.voxelMode == 0)
    {
        return false;
    }
    return isWaterTriangle(instanceDatas[instanceId], triangleIdx);
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

void applySegmentAbsorption(inout Payload payload, const float3 rayOrigin, const float3 rayDir)
{
    if (sceneParams.voxelMode == 0 || !bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        return;
    }

    const float dist = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? distance(rayOrigin, payload.hitInfo.hitPos_WS)
        : getDistanceToVoxelBounds(rayOrigin, rayDir);
    payload.pathWeight *= computeWaterAbsorption(dist);
}

void applyPassthroughAbsorption(inout Payload payload, const float rayEndT)
{
    // waterEntryT is initialized to 0 if the ray starts underwater, RAY_DEFAULT_TMAX otherwise.
    // waterExitT is initialized to RAY_DEFAULT_TMAX and updated in AnyHit on water surface hits.
    // NOTE: this correctly handles one water entry and exit (in that order) along the ray. It breaks
    // down if there are multiple distinct water bodies along the ray (e.g. two separate ponds).
    const float waterLength = max(min(payload.waterExitT, rayEndT) - payload.waterEntryT, 0.f);
    payload.pathWeight *= computeWaterAbsorption(waterLength);
}
