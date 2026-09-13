// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

int2 cloudCacheOrigin()
{
    const float2 position = cameraParams.pos_WS.xz + float2(cameraParams.globalInstanceOffset.xz)
        - float2(renderParams.cloud.windX, renderParams.cloud.windZ) * renderParams.animTime;
    return int2(floor(position * (16.f / renderParams.cloud.period))) - 64;
}

float2 cloudCacheLoad(uint2 texel)
{
    Texture2D<float4> cache = ResourceDescriptorHeap[heapIndices.srv.cloudNoiseCacheIdx];
    return cache.Load(int3(texel, 0)).xy;
}

float2 cloudVoronoiPoint(float2 cell, float time);

float2 cloudNoiseCacheValue(uint2 id)
{
    float2 value;
    if (id.y < 2)
    {
        const float time = id.y == 0
            ? renderParams.cloud.warpTime + renderParams.animTime * renderParams.cloud.warpSpeed
            : renderParams.cloud.fineTime + renderParams.animTime * renderParams.cloud.fineSpeed;
        sincos(time * (0.5f + float(id.x) / 1024.f), value.x, value.y);
    }
    else
    {
        const uint index = (id.y - 2) * 1024 + id.x;
        const int2 cell = cloudCacheOrigin() + int2(index % 128, index / 128);
        value = cloudVoronoiPoint(float2(cell),
            renderParams.cloud.voronoiTime + renderParams.animTime * renderParams.cloud.voronoiSpeed);
    }
    return value;
}
