// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#define SHARC_UPDATE 1
#define SHARC_QUERY 0
#include "sharc_common.hlsli"
cbuffer Control : REGISTER_B(SHARC, CONTROL)
{
    uint mode;
}

[numthreads(256, 1, 1)] void csMain(uint3 id
                                    : SV_DispatchThreadID)
{
    const uint i = id.x;
    SharcParameters p = makeSharcParameters();
    if (mode == 0) // clear cache
    {
        if (i >= sharcParams.capacity)
            return;
        sharcHashes[i] = 0;
        sharcAccumulation[i] = SharcZeroAccumulationData();
        sharcResolved[i] = SharcZeroPackedData();
    }
    else if (mode == 1) // temporal resolve, eviction, reset accumulation
    {
        SharcResolveParameters r = (SharcResolveParameters)0;
        r.cameraPositionPrev = sharcParams.cameraPositionPrev;
        r.accumulationFrameNum = sharcParams.accumulationFrames;
        r.staleFrameNumMax = sharcParams.staleFrames;
        r.frameIndex = sharcParams.frameIndex;
        SharcResolveEntry(i, p, r);
        if (i < sharcParams.capacity && sharcHashes[i] != 0)
            sharcCount(5);
    }
    else if (mode == 5) // per-frame diagnostic reset
    {
        if (i < 12)
            sharcStats[i] = 0;
    }
    else if (i == 0) // deterministic GPU test; exercised only by --sharcSelfTest
    {
        SharcHitData hit = makeSharcHit(float3(1, 2, 3), float3(0, 1, 0), float3(1, 1, 1));
        if (mode == 2)
        {
            SharcState state;
            SharcInit(state);
            if (SharcUpdateHit(p, state, hit, float3(0.25, 0.5, 1), 0.f))
            {
                SharcSetThroughput(state, float3(0.5, 0.5, 0.5));
                SharcUpdateMiss(p, state, float3(0.2, 0.4, 0.6));
            }
        }
        else
        {
            float3 value = 0;
            bool found = SharcGetCachedRadiance(p, hit, value, false);
            if (mode == 3)
                sharcStats[6] = found && all(abs(value - float3(0.35, 0.7, 1.3)) < 0.003f);
            else if (mode == 4)
                sharcStats[7] += !found;
        }
    }
}
