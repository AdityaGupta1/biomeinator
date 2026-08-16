// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "biome.h"

#include "chunk.h"
#include "util/glm_util.h"
#include "util/rng.h"

#include <array>
#include <limits>

#include <glm/glm.hpp>

//#define DEBUG_BIOME_OVERRIDE Biome::SAVANNA

float BiomeNoise::distance2(const BiomeNoise& other) const
{
    return (this->temperature - other.temperature) * (this->temperature - other.temperature) +
           (this->humidity - other.humidity) * (this->humidity - other.humidity) +
           (this->peak - other.peak) * (this->peak - other.peak);
}

BiomeNoise BiomeNoise::randomOffset(const BiomeNoise& base, RandomNumberGenerator& rng)
{
    return {
        .temperature = base.temperature + rng.nextFloatAbs(0.005f),
        .humidity = base.humidity + rng.nextFloatAbs(0.008f),
        .peak = base.peak + rng.nextFloatAbs(0.005f),
        .inland = base.inland + rng.nextFloatAbs(0.012f),
    };
}

namespace Biomes
{

static std::array<BiomeData, static_cast<size_t>(Biome::COUNT)> biomeDatas;

#define BIOME_DATA(biome) biomeDatas[static_cast<size_t>(biome)]
#define BIOME_DATA_BY_NAME(biomeName) biomeDatas[static_cast<size_t>(Biome::biomeName)]

static std::vector<Biome> oceanBiomes;
static std::vector<Biome> beachBiomes;
static std::vector<Biome> lowlandBiomes;
static std::vector<Biome> highlandBiomes;

void init()
{
    // ==================================================
    // OCEAN
    // ==================================================

    // OCEAN
    {
        BiomeData& data = BIOME_DATA_BY_NAME(OCEAN);
        data.name = "ocean";
        oceanBiomes.push_back(Biome::OCEAN);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = -1.0f,
        };
        data.grassTint = glmUtil::colorFromHex("#8eb971");
        data.topBlocks = {
            .top = Block::SAND,
            .mid = Block::SAND,
        };
    }

    // ==================================================
    // BEACH
    // ==================================================

    // BEACH
    {
        BiomeData& data = BIOME_DATA_BY_NAME(BEACH);
        data.name = "beach";
        beachBiomes.push_back(Biome::BEACH);
        data.biomeNoise = {
            .temperature = 0.3f,
            .humidity = 0.2f,
            .peak = -1.0f,
        };
        data.grassTint = glmUtil::colorFromHex("#a1ba68");
        data.topBlocks = {
            .top = Block::SAND,
            .mid = Block::SAND,
        };
        data.structureGens = {
            { StructureType::PALM_TREE, 32, 8 },
        };
    }

    // GRAVEL_BEACH
    {
        BiomeData& data = BIOME_DATA_BY_NAME(GRAVEL_BEACH);
        data.name = "gravel beach";
        beachBiomes.push_back(Biome::GRAVEL_BEACH);
        data.biomeNoise = {
            .temperature = -0.2f,
            .humidity = -0.2f,
            .peak = -1.0f,
        };
        data.grassTint = glmUtil::colorFromHex("#8fa470");
        data.topBlocks = {
            .top = Block::GRAVEL,
            .mid = Block::GRAVEL,
        };
    }

    // BLACK_SAND_BEACH
    {
        BiomeData& data = BIOME_DATA_BY_NAME(BLACK_SAND_BEACH);
        data.name = "black sand beach";
        beachBiomes.push_back(Biome::BLACK_SAND_BEACH);
        data.biomeNoise = {
            .temperature = -0.6f,
            .humidity = -0.3f,
            .peak = -1.0f,
        };
        data.grassTint = glmUtil::colorFromHex("#7e9152");
        data.topBlocks = {
            .top = Block::BLACK_SAND,
            .mid = Block::BLACK_SAND,
        };
    }

    // ==================================================
    // LOWLAND
    // ==================================================

    // PLAINS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(PLAINS);
        data.name = "plains";
        lowlandBiomes.push_back(Biome::PLAINS);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = -0.7f,
        };
        data.grassTint = glmUtil::colorFromHex("#91bd59");
        data.decorator.addEntry(Block::GRASS, 5.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::PINK_DAFFODIL, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 15.f);
    }

    // DESERT
    {
        BiomeData& data = BIOME_DATA_BY_NAME(DESERT);
        data.name = "desert";
        lowlandBiomes.push_back(Biome::DESERT);
        data.biomeNoise = {
            .temperature = 1.0f,
            .humidity = -1.0f,
            .peak = -0.6f,
        };
        data.grassTint = glmUtil::colorFromHex("#bfb755");
        data.topBlocks = {
            .top = Block::SAND,
            .mid = Block::SANDSTONE,
        };
        data.structureGens = {
            { StructureType::SAGUARO_CACTUS, 20, 4 },
        };
        data.decorator.addEntry(Block::DEAD_BUSH, 1.f, { Block::SAND });
        data.decorator.addEntry(Block::TINY_CACTUS, 2.f, { Block::SAND });
        data.decorator.addEntry(Block::DEAD_GRASS_1, 5.f, { Block::SAND });
        data.decorator.addEntry(Block::DEAD_GRASS_2, 5.f, { Block::SAND });
        data.decorator.addEntry(Block::AIR, 60.f);
    }

    // FOREST
    {
        BiomeData& data = BIOME_DATA_BY_NAME(FOREST);
        data.name = "forest";
        lowlandBiomes.push_back(Biome::FOREST);
        data.biomeNoise = {
            .temperature = -0.1f,
            .humidity = 0.2f,
            .peak = -0.4f,
        };
        data.grassTint = glmUtil::colorFromHex("#50a13b");
        data.structureGens = {
            {
                {
                    { StructureType::OAK_TREE, 45.f },
                    { StructureType::BIRCH_TREE, 40.f },
                    { StructureType::LARGE_OAK_TREE, 15.f },
                },
                8,
                2,
            },
        };
        data.decorator.addEntry(Block::GRASS, 4.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::PINK_DAFFODIL, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 15.f);
    }

    // TUNDRA
    {
        BiomeData& data = BIOME_DATA_BY_NAME(TUNDRA);
        data.name = "tundra";
        lowlandBiomes.push_back(Biome::TUNDRA);
        data.biomeNoise = {
            .temperature = -0.7f,
            .humidity = -0.6f,
            .peak = -0.6f,
        };
        data.grassTint = glmUtil::colorFromHex("#80b497");
        data.topBlocks = {
            .top = Block::SNOWY_GRASS_BLOCK,
            .mid = Block::DIRT,
        };
    }

    // SWAMP
    {
        BiomeData& data = BIOME_DATA_BY_NAME(SWAMP);
        data.name = "swamp";
        data.grassTint = glmUtil::colorFromHex("#78853a");
        data.decorator.addEntry(Block::GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 10.f);
    }

    // ==================================================
    // HIGHLAND
    // ==================================================

    // SAVANNA
    {
        BiomeData& data = BIOME_DATA_BY_NAME(SAVANNA);
        data.name = "savanna";
        highlandBiomes.push_back(Biome::SAVANNA);
        data.biomeNoise = {
            .temperature = 0.6f,
            .humidity = -0.6f,
            .peak = -0.2f,
        };
        data.grassTint = glmUtil::colorFromHex("#bfa243");
        data.structureGens = {
            { StructureType::ACACIA_TREE, 48, 16 },
        };
        data.decorator.addEntry(Block::GRASS, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 10.f);
    }

    // ICE_FIELDS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(ICE_FIELDS);
        data.name = "ice fields";
        highlandBiomes.push_back(Biome::ICE_FIELDS);
        data.biomeNoise = {
            .temperature = -0.85f,
            .humidity = -0.8f,
            .peak = -0.3f,
        };
        data.grassTint = glmUtil::colorFromHex("#8ab4a0");
        data.topBlocks = {
            .top = Block::SNOW,
            .mid = Block::ICE,
        };
    }

    // MOUNTAINS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(MOUNTAINS);
        data.name = "mountains";
        highlandBiomes.push_back(Biome::MOUNTAINS);
        data.biomeNoise = {
            .temperature = -0.4f,
            .humidity = -0.4f,
            .peak = 0.6f,
        };
        data.grassTint = glmUtil::colorFromHex("#6da36b");
        data.topBlocks = {
            .top = Block::STONE,
            .mid = Block::STONE,
        };
    }
}

const BiomeData& getBiomeData(Biome biome)
{
    return BIOME_DATA(biome);
}

Biome getClosestBiome(const BiomeNoise& biomeNoise)
{
#ifdef DEBUG_BIOME_OVERRIDE
    if (true)
    {
        return DEBUG_BIOME_OVERRIDE;
    }
#endif

    std::vector<Biome>* closestBiomeCandidates;
    if (biomeNoise.inland < -0.15f)
    {
        closestBiomeCandidates = &oceanBiomes;
    }
    else if (biomeNoise.inland < 0.0f)
    {
        closestBiomeCandidates = &beachBiomes;
    }
    else if (biomeNoise.inland < 0.85f)
    {
        closestBiomeCandidates = &lowlandBiomes;
    }
    else
    {
        closestBiomeCandidates = &highlandBiomes;
    }

    Biome closestBiome = Biome::COUNT;
    float closestDist2 = std::numeric_limits<float>::max();

    for (const Biome biome : *closestBiomeCandidates)
    {
        const float dist2 = biomeNoise.distance2(BIOME_DATA(biome).biomeNoise);

        if (dist2 < closestDist2)
        {
            closestBiome = biome;
            closestDist2 = dist2;
        }
    }

    ASSERT(closestBiome != Biome::COUNT);

    return closestBiome;
}

} // namespace Biomes
