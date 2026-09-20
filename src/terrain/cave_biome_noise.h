// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <limits>
#include <glm/common.hpp>

namespace CaveBiomeFields
{

inline constexpr uint32_t downsample = 4;

// The coarse field stores Y contiguously, then X, then Z. Cache XZ interpolation
// at the two Y planes so both generation and deferred decoration use the same math.
class Column
{
    const float* c00;
    const float* c10;
    const float* c01;
    const float* c11;
    float tx;
    float tz;
    uint32_t cachedY = std::numeric_limits<uint32_t>::max();
    float low = 0.f;
    float high = 0.f;

    float samplePlane(uint32_t y) const
    {
        return glm::mix(glm::mix(c00[y], c10[y], tx), glm::mix(c01[y], c11[y], tx), tz);
    }

public:
    Column(const float* coarseNoise, uint32_t coarseSizeXZ, uint32_t coarseHeight, uint32_t blockX, uint32_t blockZ)
    {
        const uint32_t x = blockX / downsample;
        const uint32_t z = blockZ / downsample;
        tx = (blockX % downsample) * (1.f / downsample);
        tz = (blockZ % downsample) * (1.f / downsample);
        c00 = coarseNoise + (z * coarseSizeXZ + x) * coarseHeight;
        c10 = c00 + coarseHeight;
        c01 = c00 + coarseSizeXZ * coarseHeight;
        c11 = c01 + coarseHeight;
    }

    float sample(uint32_t y)
    {
        const uint32_t gridY = y / downsample;
        if (gridY != cachedY)
        {
            low = cachedY != std::numeric_limits<uint32_t>::max() && gridY == cachedY + 1
                ? high : samplePlane(gridY);
            high = samplePlane(gridY + 1);
            cachedY = gridY;
        }
        const float ty = (y % downsample) * (1.f / downsample);
        return glm::mix(low, high, ty);
    }
};

} // namespace CaveBiomeFields
