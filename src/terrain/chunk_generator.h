// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block_ids.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

class ThreadMemoryAllocator;
enum class Biome : uint8_t;

namespace ChunkGenerator
{

void init();

// A column's surface as distant terrain shows it: the broad terrain shape and its top blocks, without
// detail noise, caves, structures, decorators or local water shaping (swamps, oases)
struct LodColumn
{
    int topBlockY;
    // Sub-block height where the terrain density crosses zero above the top block, as chunks keep it
    float surfaceHeight;
    int waterLevel;
    // WATER_TOP, or what covers frozen water (ICE, or a snow layer on it)
    Block waterTopBlock;
    Biome biome;
    // What the top face shows; its sides show topSideBlock, which differs where a snow layer covers it
    Block topBlock;
    Block topSideBlock;
    // Slopes too steep for the top block show topSideBlock where they drop no more than soilDepth blocks,
    // and LodRockStrata where they drop further
    int soilDepth;
};

// The rock each column's slopes show by height, sampled every stepBlocks blocks on a world-aligned grid
// from minY up to the highest top. Landform rock (Mesa terracotta, Tianzi strata) is banded by height,
// so taking each column's rock at its own top would turn the bands vertical across columns.
struct LodRockStrata
{
    int minY;
    int stepBlocks;
    uint32_t numLevels;
    const Block* blocks; // numLevels per sample

    Block atHeight(uint32_t sampleIdx, float y) const
    {
        const int level = std::clamp(static_cast<int>(std::floor((y - minY) / stepBlocks)), 0,
                                     static_cast<int>(numLevels) - 1);
        return blocks[sampleIdx * numLevels + level];
    }
};

// Samples numSamplesXZ^2 columns cellSize blocks apart starting at originXZ_WS, x-innermost. The rock strata
// live in threadMemoryAlloc.
void sampleLodColumns(glm::ivec2 originXZ_WS, int cellSize, uint32_t numSamplesXZ, LodColumn* outColumns,
                      LodRockStrata& outRockStrata, ThreadMemoryAllocator& threadMemoryAlloc);

}; // namespace ChunkGenerator
