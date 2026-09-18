// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#include "sky/cloud_shape.hlsli"

struct CloudTraversal
{
    int2 cell;
    int2 step;
    float2 nextBoundary;
    float2 delta;
    float t;
    float end;
};

CloudTraversal beginCloudTraversal(float3 origin_WS, float3 dir, float maxDistance, float layerDistance,
    bool geometryOnly = false)
{
    CloudTraversal state = (CloudTraversal)0;
    const CloudSettings c = renderParams.cloudSettings;
    if (sceneParams.voxelMode == 0 || renderParams.cloudSettings.enableClouds == 0 || c.coverage <= 0.f || (!geometryOnly && c.density <= 0.f))
    {
        return state;
    }
    const float3 origin = cloudPosition(origin_WS);
    float start = 0.f;
    float end = min(maxDistance, c.maxDistance);
    if (abs(dir.y) < 1.e-7f)
    {
        if (origin.y < c.baseHeight || origin.y >= c.baseHeight + c.thickness)
        {
            return state;
        }
    }
    else
    {
        const float a = (c.baseHeight - origin.y) / dir.y;
        const float b = (c.baseHeight + c.thickness - origin.y) / dir.y;
        start = max(0.f, min(a, b));
        end = min(end, max(a, b));
    }
    end = min(end, start + layerDistance);
    if (end <= start)
    {
        return state;
    }

    const float2 p = (origin.xz + dir.xz * start) / c.cellSize;
    state.cell = int2(floor(p));
    state.t = start;
    state.end = end;
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        const float d = dir.xz[axis];
        state.step[axis] = d > 0.f ? 1 : (d < 0.f ? -1 : 0);
        if (d < 0.f && p[axis] == float(state.cell[axis]))
        {
            --state.cell[axis];
        }
        state.delta[axis] = d != 0.f ? c.cellSize / abs(d) : 1.e30f;
        const float boundary = float(state.cell[axis] + (d > 0.f ? 1 : 0));
        state.nextBoundary[axis] = d != 0.f
            ? start + max(0.f, (boundary - p[axis]) * c.cellSize / d) : 1.e30f;
    }
    return state;
}

bool nextCloudInterval(inout CloudTraversal state, out float2 interval)
{
    interval = 0.f;
    bool found = false;
    [loop] while (state.t < state.end)
    {
        const bool occupied = cloudCellOccupied(state.cell);
        if (!occupied && found)
        {
            break;
        }
        if (occupied && !found)
        {
            interval.x = state.t;
            found = true;
        }
        const float boundary = min(state.nextBoundary.x, state.nextBoundary.y);
        state.t = min(state.end, boundary);
        [unroll] for (uint axis = 0; axis < 2; ++axis)
        {
            if (state.nextBoundary[axis] <= boundary)
            {
                state.cell[axis] += state.step[axis];
                state.nextBoundary[axis] += state.delta[axis];
            }
        }
    }
    interval.y = state.t;
    return found;
}

bool cloudSurfaceDistance(float3 origin_WS, float3 dir, float maxDistance, out float distance)
{
    CloudTraversal state = beginCloudTraversal(origin_WS, dir, maxDistance, 1.e30f, true);
    float2 interval;
    distance = 0.f;
    if (!nextCloudInterval(state, interval))
    {
        return false;
    }
    if (interval.x > 0.f)
    {
        distance = interval.x;
        return true;
    }

    // Inside a cloud, use its exit surface rather than projecting the camera position.
    const float worldY = cloudPosition(origin_WS).y;
    const float layerExit = abs(dir.y) < 1.e-7f ? 1.e30f
        : ((dir.y > 0.f ? renderParams.cloudSettings.baseHeight + renderParams.cloudSettings.thickness
            : renderParams.cloudSettings.baseHeight) - worldY) / dir.y;
    if (interval.y >= state.end && layerExit > state.end)
    {
        return false;
    }
    distance = interval.y;
    return distance > 0.f && distance < maxDistance;
}
