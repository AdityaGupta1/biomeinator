// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "sky/sky_constants.hlsli"
#include "util/rng.hlsli"

float3 cloudRandom(int3 p, int period)
{
    p = (p % period + period) % period;
    uint seed = hash(uint(p.x) ^ hash(uint(p.y) + 73u) ^ hash(uint(p.z) + 1777u));
    return float3(hash(seed), hash(seed + 1), hash(seed + 2)) / 4294967296.f;
}

float perlin(float3 uv, int cells)
{
    float3 p = uv * cells;
    int3 cell = int3(floor(p));
    float3 f = frac(p);
    float3 w = f*f*f*(f*(f*6.f-15.f)+10.f);
    float sum = 0.f;
    [unroll] for (int z=0; z<2; ++z)
    [unroll] for (int y=0; y<2; ++y)
    [unroll] for (int x=0; x<2; ++x)
    {
        int3 offset = int3(x,y,z);
        float3 g = normalize(cloudRandom(cell + offset, cells) * 2.f - 1.f);
        float3 weights = lerp(1.f-w, w, float3(offset));
        sum += dot(g, f-float3(offset)) * weights.x*weights.y*weights.z;
    }
    return saturate(sum * 0.9f + 0.5f);
}

float worley(float3 uv, int cells)
{
    float3 p = uv * cells;
    int3 cell = int3(floor(p));
    float3 f = frac(p);
    float nearest = 10.f;
    [loop] for (int z=-1; z<=1; ++z)
    [loop] for (int y=-1; y<=1; ++y)
    [loop] for (int x=-1; x<=1; ++x)
    {
        int3 offset = int3(x,y,z);
        float3 delta = float3(offset) + cloudRandom(cell+offset, cells) - f;
        nearest = min(nearest, dot(delta,delta));
    }
    return saturate(1.f - sqrt(nearest));
}

[numthreads(4,4,4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    RWTexture3D<float4> output = ResourceDescriptorHeap[lutUavIdx];
    float3 uv = (float3(id) + 0.5f) / 64.f;
    // Three octaves baked once. R is broad gradient fBM; GBA are cellular
    // octaves, combined differently for billows and erosion at runtime.
    float p = perlin(uv,2)*0.57f + perlin(uv,4)*0.28f + perlin(uv,8)*0.15f;
    output[id] = float4(p, worley(uv,8), worley(uv,16), worley(uv,32));
}
