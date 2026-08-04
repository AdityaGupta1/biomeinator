// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome.h"
#include "block.h"

#include <vector>

#include <glm/glm.hpp>

class ThreadMemoryAllocator;

namespace ChunkGenerator
{

void init();

// Batch-evaluates the surface biome noise on a uniform XZ grid (one sample per texel center,
// texelSizeBlocks blocks apart) and writes the closest biome per texel, x-innermost. Skips the
// per-column jitter chunk generation applies, so results are the macro biome field.
void fillBiomeRect(Biome* outBiomes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks);

// TEMP: scans world seeds for one with a large swamp centered near the origin. Remove along with
// the debugBool0 hook in main.cpp.
void debugScanSwampSeeds(uint32_t numSeeds);

}; // namespace ChunkGenerator
