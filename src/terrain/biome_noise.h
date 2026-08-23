// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome.h"

#include <glm/glm.hpp>

// The surface biome noise fields (temperature/humidity/peak/inland) and biome classification from
// them. Independent of chunk generation, rendering, and settings so tools can evaluate the biome
// field for a seed without linking the engine.
namespace BiomeNoiseFields
{

struct BiomeNoiseGrids
{
    float* temperature;
    float* humidity;
    float* peak;
    float* inland;
};

// A cell floods when the flood factor at its site exceeds floodCellThreshold; columns are painted
// with the swamp biome above the looser floodTintThreshold. Deliberately not 1:1 — see
// knowledge/terrain/swamp_generation.md.
inline constexpr float floodCellThreshold = 0.3f;
inline constexpr float floodTintThreshold = 0.25f;

void init(uint32_t worldSeed);

// World-space offset applied to all worldgen noise so different seeds don't share features at the
// origin; derived from the seed during init and shared with the rest of chunk generation.
glm::ivec2 getNoiseOffsetXZ();

// Batch-evaluates the four surface biome noise fields on a uniform XZ grid, x-innermost.
// startXZ already includes any sample offset (texel centers for the biome map, block corners
// for chunk generation).
void fillGrids(const BiomeNoiseGrids& grids, glm::vec2 startXZ, glm::uvec2 numSamples, float stepBlocks);

// Single-point counterpart of fillGrids for arbitrary positions (swamp cell sites).
BiomeNoise sampleAt(glm::vec2 posXZ_WS);

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx);

// Natural (pre-swamp-shaping) terrain profile of a column, derived purely from its smooth biome
// noise.
struct NaturalTerrain
{
    float baseHeight;
    float surfaceMultiplier;
};

NaturalTerrain computeNaturalTerrain(const BiomeNoise& biomeNoise);

// Continuous 0-1 flood factor: how strongly this location wants to be flooded wetland. Mid values
// give balanced water/land; values toward 1 give mostly-water terrain. Computed from smooth
// fields only, never the jittered biome — per-column jitter would give adjacent columns different
// heights/water levels.
// The inland gate keeps flooded cell sites far enough from the coast that a cell's area can't
// reach the ocean.
float computeFloodFactor(const BiomeNoise& biomeNoise);

// The swamp biome is not a Voronoi candidate; it overrides the closest biome wherever the flood
// factor is high.
Biome biomeFromNoise(const BiomeNoise& biomeNoise);

// Batch-evaluates the surface biome noise on a uniform XZ grid (one sample per texel center,
// texelSizeBlocks blocks apart) and writes the closest biome per texel, x-innermost. Skips the
// per-column jitter chunk generation applies, so results are the macro biome field.
void fillBiomeRect(Biome* outBiomes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks);

} // namespace BiomeNoiseFields
