// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "common/global_params.hlsli"
#include "sky/sky_constants.hlsli"
#include "sky/cloud_shape.hlsli"
#define CLOUD_USE_SHAPE_CACHE 0
#include "sky/cloud_density.hlsli"

[numthreads(8,8,1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<float> shape = ResourceDescriptorHeap[lutUavIdx];
    const float2 position = cloudShapeOrigin() + (float2(id.xy) + 0.5f) * cloudShapeTexelSize();
    shape[id.xy] = cloudBaseField(position * (16.f / renderParams.cloud.period));
}
