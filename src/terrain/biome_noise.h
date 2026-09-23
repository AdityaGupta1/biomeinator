// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome.h"

#include <glm/glm.hpp>

// The surface biome noise fields (temperature/humidity/peak/inland/erosion) and biome classification from
// them. Independent of chunk generation, rendering, and settings so tools (e.g. BiomeScanner) can
// evaluate the biome field for a seed without linking the engine.
namespace BiomeNoiseFields
{

struct BiomeNoiseGrids
{
    float* temperature;
    float* humidity;
    float* peak;
    float* inland;
    float* erosion;
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

// Batch-evaluates the surface biome noise fields on a uniform XZ grid, x-innermost.
// startXZ already includes any sample offset (texel centers for the biome map, block corners
// for chunk generation).
void fillGrids(const BiomeNoiseGrids& grids, glm::vec2 startXZ, glm::uvec2 numSamples, float stepBlocks);

// Batch counterpart of sampleAt for arbitrary positions.
void fillPositions(const BiomeNoiseGrids& grids, const float* xPositions, const float* zPositions, uint32_t numSamples);

// Single-point counterpart of fillGrids for arbitrary positions (swamp cell sites).
BiomeNoise sampleAt(glm::vec2 posXZ_WS);

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx);

// Natural terrain before local water shaping. Uses smooth biome noise and world-space
// formations; independent of chunk resolution, biome labels, and generation order.
struct NaturalTerrain
{
    float baseHeight;
    float surfaceMultiplier;
    // Ground before formations and their contribution, used to expose rock/quartz without
    // painting isolated structures or extending surface materials through deep cave biomes.
    float formationBaseHeight;
    float formationHeight;
    // The supporting Worley site's identity lets materials vary by pillar without
    // deriving their layers from the per-column surface height.
    glm::ivec2 formationSite{};
};

NaturalTerrain computeNaturalTerrain(const BiomeNoise& biomeNoise, glm::vec2 posXZ_WS);

float dryClimateWeight(const BiomeNoise& noise);
// Preserved relief away from the coast, 0 near the shore. Terrain scales its mountain relief by
// this (further limited to non-dry climates); the biome search uses it to choose highland
// candidates, so highland labels only extend toward the coast where relief does.
float highlandReliefWeight(const BiomeNoise& noise);

// Biomes whose label must agree with a landform. Each regime combines its climate, erosion and
// inland axes into one suitability; the regime claims the label where suitability exceeds its
// threshold, checked in enum (priority) order before the nearest-climate search. Suitabilities
// are zero on the coast (inland < 0), so regimes never claim ocean or beach.
enum class TerrainRegime : uint8_t
{
    SWAMP,
    TIANZI,
    MESA,
    RED_DESERT,

    COUNT
};

// Terrain strength of a regime's landform: 0 at its label threshold, ramping to 1 at full
// strength, and also 0 wherever a higher-priority regime claims the label. A landform therefore
// never extends past its label (up to per-column jitter at the border).
float regimeWeight(TerrainRegime regime, const BiomeNoise& noise);
float surfaceDetailWeight(const BiomeNoise& noise);

// Continuous 0-1 flood factor: how strongly this location wants to be flooded wetland. Mid values
// give balanced water/land; values toward 1 give mostly-water terrain. Computed from smooth
// fields only, never the jittered biome — per-column jitter would give adjacent columns different
// heights/water levels.
// The inland gate keeps flooded cell sites far enough from the coast that a cell's area can't
// reach the ocean.
float computeFloodFactor(const BiomeNoise& biomeNoise);

// The first terrain regime that claims the column, otherwise the closest climate candidate.
Biome biomeFromNoise(const BiomeNoise& biomeNoise);

// Batch-evaluates the surface biome noise on a uniform XZ grid (one sample per texel center,
// texelSizeBlocks blocks apart) and writes the closest biome per texel, x-innermost. Skips the
// per-column jitter chunk generation applies, so results are the macro biome field.
void fillBiomeRect(Biome* outBiomes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks);

} // namespace BiomeNoiseFields
