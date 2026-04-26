// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_params.h"
#include "../rendering/common/common_registers.h"

cbuffer GlobalParams : REGISTER_B(COMMON, GLOBAL_PARAMS)
{
    HeapIndices heapIndices;
    ConstantParams constantParams;
    CameraParams cameraParams;
    SceneParams sceneParams;
    RenderParams renderParams;
    DebugParams debugParams;
};

RWTexture2D<float4> debugTexture()
{
    return ResourceDescriptorHeap[heapIndices.uav.debugTargetIdx];
}
