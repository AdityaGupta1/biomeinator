// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "cave_biome.h"

#include "debug.h"

#include <array>
#include <limits>

float CaveBiomeNoise::distance2(const CaveBiomeNoise& other) const
{
    return (this->temperature - other.temperature) * (this->temperature - other.temperature) +
           (this->humidity - other.humidity) * (this->humidity - other.humidity);
}

namespace CaveBiomes
{

static std::array<CaveBiomeData, static_cast<size_t>(CaveBiome::COUNT)> caveBiomeDatas;

#define CAVE_BIOME_DATA(caveBiome) caveBiomeDatas[static_cast<size_t>(caveBiome)]
#define CAVE_BIOME_DATA_BY_NAME(caveBiomeName) caveBiomeDatas[static_cast<size_t>(CaveBiome::caveBiomeName)]

void init()
{
    // STONE sits at the origin so it only wins near the center of noise space; the
    // special biomes sit at the extremes and only appear where the noise is strong.

    // STONE
    {
        CaveBiomeData& data = CAVE_BIOME_DATA_BY_NAME(STONE);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
        };
        data.baseBlock = Block::STONE;
        data.caveStructureGens = {
            { .type = CaveStructureType::STONE_COLUMN, .minLayerHeight = 10, .gridCellSideLength = 56, .gridCellPadding = 12 },
        };
    }

    // LUSH
    {
        CaveBiomeData& data = CAVE_BIOME_DATA_BY_NAME(LUSH);
        data.biomeNoise = {
            .temperature = 0.7f,
            .humidity = 0.7f,
        };
        data.baseBlock = Block::MOSS;
    }

    // BRIMSTONE
    {
        CaveBiomeData& data = CAVE_BIOME_DATA_BY_NAME(BRIMSTONE);
        data.biomeNoise = {
            .temperature = 0.7f,
            .humidity = -0.7f,
        };
        data.baseBlock = Block::HELLSTONE;
        data.caveStructureGens = {
            { .type = CaveStructureType::OBSIDIAN_STALACTITE, .generatesFromCeiling = true, .minLayerHeight = 15, .gridCellSideLength = 8, .gridCellPadding = 2 },
        };
    }

    // MARBLE_CRYSTALS
    {
        CaveBiomeData& data = CAVE_BIOME_DATA_BY_NAME(MARBLE_CRYSTALS);
        data.biomeNoise = {
            .temperature = -0.7f,
            .humidity = -0.7f,
        };
        data.baseBlock = Block::MARBLE;
        data.caveStructureGens = {
            { .type = CaveStructureType::CRYSTAL, .minLayerHeight = 12, .gridCellSideLength = 12, .gridCellPadding = 4 },
        };
    }

    // SUBMARINE
    {
        CaveBiomeData& data = CAVE_BIOME_DATA_BY_NAME(SUBMARINE);
        data.biomeNoise = {
            .temperature = -0.7f,
            .humidity = 0.7f,
        };
        data.baseBlock = Block::SCALESTONE;
    }
}

const CaveBiomeData& getCaveBiomeData(CaveBiome caveBiome)
{
    return CAVE_BIOME_DATA(caveBiome);
}

CaveBiome getClosestCaveBiome(const CaveBiomeNoise& caveBiomeNoise)
{
    CaveBiome closestCaveBiome = CaveBiome::COUNT;
    float closestDist2 = std::numeric_limits<float>::max();

    for (size_t i = 0; i < static_cast<size_t>(CaveBiome::COUNT); ++i)
    {
        const float dist2 = caveBiomeNoise.distance2(caveBiomeDatas[i].biomeNoise);

        if (dist2 < closestDist2)
        {
            closestCaveBiome = static_cast<CaveBiome>(i);
            closestDist2 = dist2;
        }
    }

    ASSERT(closestCaveBiome != CaveBiome::COUNT);

    return closestCaveBiome;
}

} // namespace CaveBiomes
