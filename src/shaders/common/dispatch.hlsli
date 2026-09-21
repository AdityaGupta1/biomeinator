// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_settings.h"

// Flat thread index of a workload dispatched with Util::calculateDispatchSize2D, whose group
// count wraps into y once x reaches the per-dimension limit
uint flatDispatchThreadIdx(uint3 dispatchThreadId, uint threadGroupSize)
{
    return dispatchThreadId.y * (DISPATCH_MAX_GROUPS_PER_DIM * threadGroupSize) + dispatchThreadId.x;
}
