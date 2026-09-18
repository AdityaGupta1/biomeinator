// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "common/global_params.hlsli"
#include "sky/sky_constants.hlsli"
#include "sky/cloud_shape.hlsli"

[numthreads(8,8,1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<uint> shape = ResourceDescriptorHeap[lutUavIdx];
    shape[id.xy] = cloudCellOccupancy(cloudShapeOrigin() + int2(id.xy));
}
