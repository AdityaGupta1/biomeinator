// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "../rendering/common/common_settings.h"

#include "common/global_params.hlsli"
#include "sky/cloud_occupancy.hlsli"
#include "sky/sky_constants.hlsli"

// Caches one occupancy value per grid cell in a window around the camera so cloud traversal
// can replace three octaves of noise per visited cell with one integer load.

[shader("compute")]
[numthreads(SKY_WORKGROUP_SIZE_X, SKY_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2D<uint> occupancyMap = ResourceDescriptorHeap[lutUavIdx];
    const int2 cell = cloudOccupancyMapOrigin() + int2(dispatchThreadId.xy);
    occupancyMap[dispatchThreadId.xy] = computeCloudCellOccupancy(cell) ? 1 : 0;
}
