// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

static const uint cloudShapeSize = 512;

#ifndef CLOUD_USE_SHAPE_CACHE
#define CLOUD_USE_SHAPE_CACHE 1
#endif

float3 cloudPosition(float3 position_WS)
{
    float3 p = position_WS + float3(cameraParams.globalInstanceOffset);
    p.xz -= float2(renderParams.cloud.windX, renderParams.cloud.windZ) * renderParams.animTime;
    return p;
}

int2 cloudShapeOrigin()
{
    return int2(floor(cloudPosition(cameraParams.pos_WS).xz / renderParams.cloud.cellSize))
        - int(cloudShapeSize / 2);
}

float cloudHash(int2 cell)
{
    uint h = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u ^ renderParams.cloud.seed;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.f / 16777216.f);
}

float cloudNoise(float2 p)
{
    const int2 cell = int2(floor(p));
    float2 f = frac(p);
    f = f * f * (3.f - 2.f * f);
    return lerp(lerp(cloudHash(cell), cloudHash(cell + int2(1, 0)), f.x),
        lerp(cloudHash(cell + int2(0, 1)), cloudHash(cell + 1), f.x), f.y);
}

uint cloudCellOccupancy(int2 cell)
{
    const float coverage = renderParams.cloud.coverage;
    if (coverage <= 0.f) return 0;
    if (coverage >= 1.f) return 1;
    const float2 p = (float2(cell) + 0.5f) * (renderParams.cloud.cellSize / renderParams.cloud.period);
    const float noise = (cloudNoise(p) + 0.5f * cloudNoise(p * 2.f + 17.3f)
        + 0.25f * cloudNoise(p * 4.f - 9.1f)) / 1.75f;
    // Expand the clustered noise values to give the coverage control a useful range.
    return noise > lerp(1.f, 0.f, coverage) * 0.5f + 0.25f ? 1 : 0;
}

bool cloudCellOccupied(int2 cell)
{
#if CLOUD_USE_SHAPE_CACHE
    const int2 texel = cell - cloudShapeOrigin();
    if (all(texel >= 0) && all(texel < int(cloudShapeSize)))
    {
        Texture2D<uint> shape = ResourceDescriptorHeap[heapIndices.srv.cloudShapeIdx];
        return shape.Load(int3(texel, 0)) != 0;
    }
#endif
    return cloudCellOccupancy(cell) != 0;
}
