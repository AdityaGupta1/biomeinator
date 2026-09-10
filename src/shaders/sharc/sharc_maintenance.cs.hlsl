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
    if (mode == SHARC_MAINTENANCE_CLEAR) // clear cache
    {
        if (i >= sharcParams.capacity)
            return;
        sharcHashes[i] = 0;
        sharcAccumulation[i] = SharcZeroAccumulationData();
        sharcResolved[i] = SharcZeroPackedData();
    }
    else if (mode == SHARC_MAINTENANCE_RESOLVE) // temporal resolve, eviction, reset accumulation
    {
        SharcResolveParameters r = (SharcResolveParameters)0;
        r.cameraPositionPrev = sharcParams.cameraPositionPrev;
        r.accumulationFrameNum = sharcParams.accumulationFrames;
        r.staleFrameNumMax = sharcParams.staleFrames;
        r.frameIndex = sharcParams.frameIndex;
        SharcResolveEntry(i, p, r);
    }
}
