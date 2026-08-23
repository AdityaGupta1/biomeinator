// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome.h"

#include <glm/glm.hpp>

// The surface biome noise fields (temperature/humidity/peak/inland) and biome classification from
// them. Independent of chunk generation, rendering, and settings so tools can evaluate the biome
// field for a seed without linking the engine.
namespace BiomeNoiseField
{

struct BiomeNoiseGrids
{
    float* temperature;
    float* humidity;
    float* peak;
    float* inland;
};

void init(uint32_t worldSeed);

// World-space offset applied to all worldgen noise so different seeds don't share features at the
// origin; derived from the seed during init and shared with the rest of chunk generation.
glm::ivec2 getNoiseOffsetXZ();

// Batch-evaluates the four surface biome noise fields on a uniform XZ grid, x-innermost.
// startXZ already includes any sample offset (texel centers for the biome map, block corners
// for chunk generation).
void fillGrids(const BiomeNoiseGrids& grids, glm::vec2 startXZ, glm::uvec2 numSamples, float stepBlocks);

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx);

// Batch-evaluates the surface biome noise on a uniform XZ grid (one sample per texel center,
// texelSizeBlocks blocks apart) and writes the closest biome per texel, x-innermost. Skips the
// per-column jitter chunk generation applies, so results are the macro biome field.
void fillBiomeRect(Biome* outBiomes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks);

} // namespace BiomeNoiseField
