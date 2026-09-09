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
    // themed biomes sit toward the extremes and only appear where the noise is strong.

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
            .temperature = 0.3f,
            .humidity = 0.3f,
        };
        data.baseBlock = Block::STONE;
        data.secondaryBaseBlock = Block::MARBLE;
        data.skinBlock = Block::MOSS;
        data.skinPatchBlock = Block::CLAY;
        data.skinFringeBlock = Block::OVERGROWN_STONE;
        data.secondarySkinFringeBlock = Block::OVERGROWN_MARBLE;
        data.scatterLamps = false;
        data.caveStructureGens = {
            { .type = CaveStructureType::LAMP_CLUSTER, .generatesFromCeiling = true, .minLayerHeight = 12, .gridCellSideLength = 18, .gridCellPadding = 6, .chance = 0.6f },
            { .type = CaveStructureType::CAVE_VINES, .generatesFromCeiling = true, .minLayerHeight = 8, .gridCellSideLength = 8, .gridCellPadding = 2, .chance = 0.5f },
            { .type = CaveStructureType::MOSS_PINK_CLUSTER, .minLayerHeight = 3, .gridCellSideLength = 10, .gridCellPadding = 2, .chance = 0.6f },
        };
        data.decorator.addEntry(Block::FERN, 4.f, { Block::MOSS, Block::OVERGROWN_STONE, Block::OVERGROWN_MARBLE });
        data.decorator.addEntry(Block::GLOWSHROOM_YELLOW, 1.f);
        data.decorator.addEntry(Block::AIR, 32.f);
    }

    // CRYSTALS: cool and dry, mirroring LUSH through the origin
    {
        CaveBiomeData& data = CAVE_BIOME_DATA_BY_NAME(CRYSTALS);
        data.biomeNoise = {
            .temperature = -0.3f,
            .humidity = -0.3f,
        };
        data.baseBlock = Block::BASALT;
        data.flatSurfaceBlock = Block::CRACKED_BASALT;
        data.secondaryBaseBlock = Block::STONE;
        data.scatterLamps = false;
        data.caveStructureGens = {
            { .type = CaveStructureType::CRYSTAL_CLUSTER, .minLayerHeight = 8, .gridCellSideLength = 24, .gridCellPadding = 6, .chance = 0.6f },
            { .type = CaveStructureType::CRYSTAL_CLUSTER_HANGING, .generatesFromCeiling = true, .minLayerHeight = 8, .gridCellSideLength = 24, .gridCellPadding = 6, .chance = 0.6f },
        };
    }
}

const CaveBiomeData& getCaveBiomeData(CaveBiome caveBiome)
{
    return CAVE_BIOME_DATA(caveBiome);
}

bool isCaveFloraGroundBlock(Block block)
{
    return block == Block::MOSS || block == Block::OVERGROWN_STONE || block == Block::OVERGROWN_MARBLE;
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
