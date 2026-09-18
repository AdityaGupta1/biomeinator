// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_settings.h"

#include "common/global_params.hlsli"

// Position in the cloud grid's frame: true world space with the wind translation removed.
float3 cloudPosition(const float3 position_WS)
{
    float3 p = position_WS + float3(cameraParams.globalInstanceOffset);
    p.xz -= float2(renderParams.cloudSettings.windX, renderParams.cloudSettings.windZ) * renderParams.animTime;
    return p;
}

// Grid cell stored at texel (0, 0) of the occupancy map, which is centered on the camera.
int2 cloudOccupancyMapOrigin()
{
    return int2(floor(cloudPosition(cameraParams.pos_WS).xz / renderParams.cloudSettings.cellSize))
        - int(CLOUD_OCCUPANCY_MAP_SIZE / 2);
}

float cloudHash(const int2 cell)
{
    uint h = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u ^ renderParams.cloudSettings.seed;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.f / 16777216.f);
}

float cloudNoise(const float2 p)
{
    const int2 cell = int2(floor(p));
    float2 f = frac(p);
    f = f * f * (3.f - 2.f * f);
    return lerp(lerp(cloudHash(cell), cloudHash(cell + int2(1, 0)), f.x),
        lerp(cloudHash(cell + int2(0, 1)), cloudHash(cell + 1), f.x), f.y);
}

bool computeCloudCellOccupancy(const int2 cell)
{
    const float coverage = renderParams.cloudSettings.coverage;
    if (coverage <= 0.f)
    {
        return false;
    }
    if (coverage >= 1.f)
    {
        return true;
    }
    const float2 p = (float2(cell) + 0.5f) * (renderParams.cloudSettings.cellSize / renderParams.cloudSettings.patternScale);
    const float noise = (cloudNoise(p) + 0.5f * cloudNoise(p * 2.f + 17.3f) + 0.25f * cloudNoise(p * 4.f - 9.1f)) / 1.75f;
    // The summed noise clusters around 0.5, so the coverage control sweeps the threshold over
    // the 0.75-0.25 band rather than the full 1-0 range.
    return noise > 0.75f - 0.5f * coverage;
}

// Reads the per-frame occupancy map when the cell is inside its window and evaluates the
// noise directly otherwise, so leaving the window never clips or repeats the pattern.
bool isCloudCellOccupied(const int2 cell)
{
    const int2 texel = cell - cloudOccupancyMapOrigin();
    if (all(texel >= 0) && all(texel < CLOUD_OCCUPANCY_MAP_SIZE))
    {
        Texture2D<uint> occupancyMap = ResourceDescriptorHeap[heapIndices.srv.cloudOccupancyIdx];
        return occupancyMap.Load(int3(texel, 0)) != 0;
    }
    return computeCloudCellOccupancy(cell);
}
