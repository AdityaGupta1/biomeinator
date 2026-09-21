// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_params.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "common/dispatch.hlsli"

// Compacts the area light sampling structure after instances left the TLAS: thread i writes
// element i of the compacted array, finding the surviving block it belongs to by binary
// search on newOffset and copying from that block's old position
cbuffer AreaLightCompactConstants : REGISTER_B(AREA_LIGHT_COMPACT, CONSTANTS)
{
    uint numRanges;
    uint numElements;
};

StructuredBuffer<AreaLightCompactRange> ranges : REGISTER_T(AREA_LIGHT_COMPACT, RANGES);
StructuredBuffer<uint> src : REGISTER_T(AREA_LIGHT_COMPACT, SRC);
RWStructuredBuffer<uint> dst : REGISTER_U(AREA_LIGHT_COMPACT, DST);

[shader("compute")]
[numthreads(AREA_LIGHT_COMPACT_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint elementIdx = flatDispatchThreadIdx(dispatchThreadId, AREA_LIGHT_COMPACT_WORKGROUP_SIZE);
    if (elementIdx >= numElements)
    {
        return;
    }

    uint lo = 0;
    uint hi = numRanges - 1;
    while (lo < hi)
    {
        const uint mid = (lo + hi + 1) / 2;
        if (ranges[mid].newOffset <= elementIdx)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    const AreaLightCompactRange range = ranges[lo];

    dst[elementIdx] = src[range.oldOffset + (elementIdx - range.newOffset)];
}
