// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block_ids.h"

#include <glm/glm.hpp>

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
    int waterLevel;
    // WATER_TOP, or what covers frozen water (ICE, or a snow layer on it)
    Block waterTopBlock;
    Biome biome;
    // What the top face shows; its sides show topSideBlock, which differs where a snow layer covers it
    Block topBlock;
    Block topSideBlock;
    // Shown by cliffs below the top block down to soilDepth blocks below the top, LodRockStrata under it
    Block soilBlock;
    int soilDepth;
};

// The rock each column's cliffs show by height, sampled every stepBlocks blocks on a world-aligned grid
// from minY. Landform rock (Mesa terracotta, Tianzi strata) is banded by height, so a cliff taking one
// rock at its own top would turn the bands vertical across columns.
struct LodRockStrata
{
    int minY;
    int stepBlocks;
    uint32_t numLevels;
    const Block* blocks; // numLevels per sample

    int levelOf(int y) const
    {
        return (y - minY) / stepBlocks;
    }

    int levelBottomY(int level) const
    {
        return minY + level * stepBlocks;
    }

    Block at(uint32_t sampleIdx, int level) const
    {
        return blocks[sampleIdx * numLevels + level];
    }
};

// Samples numSamplesXZ^2 columns cellSize blocks apart starting at originXZ_WS, x-innermost. The rock strata
// reach strataDepth blocks below the lowest top, and live in threadMemoryAlloc.
void sampleLodColumns(glm::ivec2 originXZ_WS, int cellSize, uint32_t numSamplesXZ, int strataDepth,
                      LodColumn* outColumns, LodRockStrata& outRockStrata, ThreadMemoryAllocator& threadMemoryAlloc);

}; // namespace ChunkGenerator
