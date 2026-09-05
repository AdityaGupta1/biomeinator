// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "common/global_params.hlsli"

StructuredBuffer<uint> reservoirSeedsIn : REGISTER_T(RESTIR_DUP, RESERVOIR_SEEDS_IN);
RWStructuredBuffer<float> duplicationMapOut : REGISTER_U(RESTIR_DUP, DUPLICATION_MAP_OUT);

// Correlation measure of ReSTIR PT Enhanced (Lin et al. 2026, Section 5): the fraction of the
// surrounding 17x17 reservoirs holding a shifted copy of the same initial sample, detected by the
// path seed they carry for random replay. Next frame's temporal pass lowers the confidence cap
// where this is high, so a firefly cannot keep spreading through reuse. Empty reservoirs (seed 0)
// never count.
[shader("compute")]
[numthreads(RESTIR_DUPLICATION_WORKGROUP_SIZE_X, RESTIR_DUPLICATION_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 pixelIdx = int2(dispatchThreadId.xy);
    const int2 size = int2(renderParams.renderSize);
    if (pixelIdx.x >= size.x || pixelIdx.y >= size.y)
    {
        return;
    }
    const uint linearPixelIdx = uint(pixelIdx.y * size.x + pixelIdx.x);

    const uint seed = reservoirSeedsIn[linearPixelIdx];
    if (seed == 0)
    {
        duplicationMapOut[linearPixelIdx] = 0.f;
        return;
    }

    uint duplicates = 0;
    const int radius = RESTIR_DUPLICATION_RADIUS;
    for (int dy = -radius; dy <= radius; ++dy)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const int2 tap = pixelIdx + int2(dx, dy);
            if ((dx == 0 && dy == 0) || any(tap < 0) || any(tap >= size))
            {
                continue;
            }
            duplicates += (reservoirSeedsIn[uint(tap.y * size.x + tap.x)] == seed) ? 1u : 0u;
        }
    }

    const float windowMinusSelf = float((2 * radius + 1) * (2 * radius + 1) - 1);
    duplicationMapOut[linearPixelIdx] = float(duplicates) / windowMinusSelf;
}
