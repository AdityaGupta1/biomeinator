// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome.h"

#include <glm/glm.hpp>

#include <array>

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

    static constexpr uint32_t numFields = 5;

    // Views one caller-owned buffer of numFields * numSamples floats, one field after another.
    static BiomeNoiseGrids fromBuffer(float* data, uint32_t numSamples)
    {
        return {
            .temperature = data,
            .humidity = data + numSamples,
            .peak = data + 2 * numSamples,
            .inland = data + 3 * numSamples,
            .erosion = data + 4 * numSamples,
        };
    }
};

// A cell floods when the flood factor at its site exceeds floodCellThreshold; columns are painted
// with the swamp biome above the looser floodTintThreshold. Deliberately not 1:1 — see
// knowledge/terrain/swamp_generation.md.
inline constexpr float floodCellThreshold = 0.3f;
inline constexpr float floodTintThreshold = 0.25f;
// Flood factor at which pond floors reach full depth; also the swamp regime's full strength.
inline constexpr float floodFullStrength = 0.9f;

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

// Single-point counterpart of fillGrids for arbitrary positions.
BiomeNoise sampleAt(glm::vec2 posXZ_WS);

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx);

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

// One weight per regime. Every weight is 0 wherever a higher-priority regime claims the label.
struct RegimeWeights
{
    std::array<float, static_cast<size_t>(TerrainRegime::COUNT)> weights{};

    float operator[](TerrainRegime regime) const
    {
        return weights[static_cast<size_t>(regime)];
    }
};

// Whether a terrain regime claims this column's label. Regime landforms (weight > 0) only exist
// inside their labels, so this also answers whether any landform is present.
bool isClaimedByRegime(const BiomeNoise& noise);

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
    // Landform strength: 0 at each regime's label threshold, ramping to 1 at full strength.
    RegimeWeights regimeWeights{};
    // 1 across each regime's whole label, fading out just outside it. Yes/no materials use this;
    // the landform ramp would leave the outer band of the label with foreign rock.
    RegimeWeights regimeCoverage{};
    // Noise-driven style weight for regimes with their own roughness: the same factors as the
    // label, through softer ramps, full by the label edge and fading well beyond it. Continuous
    // styles (roughness, fine detail) use this so they never change abruptly at a label edge.
    RegimeWeights regimeStyle{};

    // Whether a regime's yes/no styles (rock materials) apply here. Half coverage sits just
    // outside the label; a lower cutoff would spread them deep into neighboring biomes.
    bool isCoveredBy(TerrainRegime regime) const
    {
        return regimeCoverage[regime] >= 0.5f;
    }
};

NaturalTerrain computeNaturalTerrain(const BiomeNoise& biomeNoise, glm::vec2 posXZ_WS);

// widen stretches the ramps about their centers for softer style blends (1 = the label ramps).
float dryClimateWeight(const BiomeNoise& noise, float widen = 1.f);
// Preserved relief: 1 where erosion keeps dramatic landforms, 0 in eroded, flat terrain.
float ruggedWeight(const BiomeNoise& noise, float widen = 1.f);
// Strength of the tall peak relief terrain raises in preserved highlands, 0-1: preserved relief
// away from the coast, times a steep response to peak.
float mountainPeakWeight(const BiomeNoise& noise);
// Highland candidates are chosen where mountainPeakWeight is substantial, so highland labels sit
// exactly where mountain relief does: not on low-peak rugged ground, not ahead of relief at coasts.
bool isHighland(const BiomeNoise& noise);

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
