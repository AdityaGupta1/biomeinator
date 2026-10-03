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
    Biome biome;
    Block topBlock;
    // Shown by cliffs: soilBlock for the first soilDepth blocks below the top, rockBlock under it
    Block soilBlock;
    int soilDepth;
    Block rockBlock;
};

// Samples numSamplesXZ^2 columns cellSize blocks apart starting at originXZ_WS, x-innermost
void sampleLodColumns(glm::ivec2 originXZ_WS, int cellSize, uint32_t numSamplesXZ, LodColumn* outColumns,
                      ThreadMemoryAllocator& threadMemoryAlloc);

}; // namespace ChunkGenerator
