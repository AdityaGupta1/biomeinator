// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "sky/cloud_occupancy.hlsli"

// Distance limit meaning "no limit"; larger than any draw or shadow distance.
static const float cloudUnboundedDistance = 1e30f;

// 2D DDA state over the cloud grid along one ray, clipped to the layer's height range.
struct CloudTraversal
{
    int2 cell;
    int2 step;
    float2 nextBoundary;
    float2 delta;
    float t;
    float end;
    float layerExit; // where the ray leaves the layer's height range, ignoring the other limits
};

// Clips the ray to the layer and to min(maxDistance, draw distance, layer entry + layerDistance).
// ignoreExtinction keeps traversing at zero extinction, for the G-buffer's cloud surface.
CloudTraversal beginCloudTraversal(const float3 origin_WS, const float3 dir, const float maxDistance,
    const float layerDistance, const bool ignoreExtinction = false)
{
    CloudTraversal state = (CloudTraversal)0;
    const CloudSettings c = renderParams.cloudSettings;
    if (sceneParams.voxelMode == 0 || c.enableClouds == 0 || c.coverage <= 0.f
        || (!ignoreExtinction && c.extinction <= 0.f))
    {
        return state;
    }

    const float3 origin = cloudPosition(origin_WS);
    float start = 0.f;
    float end = min(maxDistance, c.drawDistance);
    if (abs(dir.y) < 1e-7f)
    {
        if (origin.y < c.baseHeight || origin.y >= c.baseHeight + c.thickness)
        {
            return state;
        }
        state.layerExit = cloudUnboundedDistance;
    }
    else
    {
        const float a = (c.baseHeight - origin.y) / dir.y;
        const float b = (c.baseHeight + c.thickness - origin.y) / dir.y;
        start = max(0.f, min(a, b));
        state.layerExit = max(a, b);
        end = min(end, state.layerExit);
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
        state.delta[axis] = d != 0.f ? c.cellSize / abs(d) : cloudUnboundedDistance;
        const float boundary = float(state.cell[axis] + (d > 0.f ? 1 : 0));
        state.nextBoundary[axis] = d != 0.f
            ? start + max(0.f, (boundary - p[axis]) * c.cellSize / d) : cloudUnboundedDistance;
    }
    return state;
}

// Advances to the next run of adjacent occupied cells and returns it as one [t0, t1) interval.
bool nextCloudInterval(inout CloudTraversal state, out float2 interval)
{
    interval = 0.f;
    bool found = false;
    [loop] while (state.t < state.end)
    {
        const bool occupied = isCloudCellOccupied(state.cell);
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

// First cloud boundary along the ray, for the G-buffer's depth and motion vectors. Starting
// inside a cloud, it is the exit boundary, which may be the top or bottom of the layer; a
// geometry endpoint or the draw distance is never reported as a surface.
bool cloudSurfaceDistance(const float3 origin_WS, const float3 dir, const float maxDistance, out float surfaceDistance)
{
    surfaceDistance = 0.f;
    CloudTraversal state = beginCloudTraversal(origin_WS, dir, maxDistance, cloudUnboundedDistance, true);
    float2 interval;
    if (!nextCloudInterval(state, interval))
    {
        return false;
    }
    if (interval.x > 0.f)
    {
        surfaceDistance = interval.x;
        return true;
    }
    const bool cutByLayer = state.end >= state.layerExit;
    if (interval.y >= state.end && !cutByLayer)
    {
        return false;
    }
    surfaceDistance = interval.y;
    return true;
}
