// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "biome.h"
#include "biome_noise.h"
#include "formation_rock.h"

#include "util/glm_util.h"
#include "util/rng.h"

#include <array>
#include <limits>

#include <glm/glm.hpp>

//#define DEBUG_BIOME_OVERRIDE Biome::SAVANNA

BiomeNoise BiomeNoise::randomOffset(const BiomeNoise& base, RandomNumberGenerator& rng)
{
    return {
        .temperature = base.temperature + rng.nextFloatAbs(0.005f),
        .humidity = base.humidity + rng.nextFloatAbs(0.008f),
        .peak = base.peak + rng.nextFloatAbs(0.005f),
        .inland = base.inland + rng.nextFloatAbs(0.012f),
        .erosion = base.erosion + rng.nextFloatAbs(0.006f),
    };
}

namespace Biomes
{

static std::array<BiomeData, static_cast<size_t>(Biome::COUNT)> biomeDatas;

#define BIOME_DATA(biome) biomeDatas[static_cast<size_t>(biome)]
#define BIOME_DATA_BY_NAME(biomeName) biomeDatas[static_cast<size_t>(Biome::biomeName)]
#define BIOME_INIT(biomeName, displayName) \
    BiomeData& data = BIOME_DATA_BY_NAME(biomeName); \
    data.name = displayName

static std::array<std::vector<Biome>, static_cast<size_t>(BiomeTier::COUNT)> candidatesByTier;

void init()
{
    biomeDatas = {};
    // ==================================================
    // OCEAN
    // ==================================================

    // OCEAN
    {
        BIOME_INIT(OCEAN, "ocean");
        data.tier = BiomeTier::OCEAN;
        data.climate = { .temperature = 0.0f, .humidity = 0.0f };
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
        BIOME_INIT(BEACH, "beach");
        data.tier = BiomeTier::BEACH;
        data.climate = { .temperature = 0.3f, .humidity = 0.2f };
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
        BIOME_INIT(GRAVEL_BEACH, "gravel beach");
        data.tier = BiomeTier::BEACH;
        data.climate = { .temperature = -0.2f, .humidity = -0.2f };
        data.grassTint = glmUtil::colorFromHex("#8fa470");
        data.topBlocks = {
            .top = Block::GRAVEL,
            .mid = Block::GRAVEL,
        };
    }

    // BLACK_SAND_BEACH
    {
        BIOME_INIT(BLACK_SAND_BEACH, "black sand beach");
        data.tier = BiomeTier::BEACH;
        data.climate = { .temperature = -0.6f, .humidity = -0.3f };
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
        BIOME_INIT(PLAINS, "plains");
        data.tier = BiomeTier::LOWLAND;
        data.climate = { .temperature = 0.1f, .humidity = -0.1f };
        data.grassTint = glmUtil::colorFromHex("#91bd59");
        data.decorator.addEntry(Block::GRASS, 5.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::PINK_DAFFODIL, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 15.f);
    }

    // DESERT
    {
        BIOME_INIT(DESERT, "desert");
        data.tier = BiomeTier::LOWLAND;
        data.climate = { .temperature = 0.5f, .humidity = -0.4f };
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
        BIOME_INIT(FOREST, "forest");
        data.tier = BiomeTier::LOWLAND;
        data.climate = { .temperature = -0.15f, .humidity = 0.25f };
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
        BIOME_INIT(TUNDRA, "tundra");
        data.tier = BiomeTier::LOWLAND;
        data.climate = { .temperature = -0.5f, .humidity = -0.35f };
        data.grassTint = glmUtil::colorFromHex("#80b497");
        // PROTOTYPE placement for snow layers: grass under a mostly-continuous snow sheet with bare
        // patches. The intended home is the snow line's snowy-grass band, once that lands.
        data.topBlocks = {
            .top = Block::GRASS_BLOCK,
            .mid = Block::DIRT,
        };
        data.decorator.addEntry(Block::SNOW_LAYER, 80.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 20.f);
    }

    // SWAMP
    {
        BIOME_INIT(SWAMP, "swamp");
        data.grassTint = glmUtil::colorFromHex("#78853a");
        data.topBlocks = {
            .underwaterTop = Block::MUD,
            .shoreTop = Block::MUD,
        };
        data.structureGens = {
            { StructureType::CYPRESS_TREE, 23, 4, STRUCTURE_GEN_FLAG_ALLOW_UNDERWATER },
        };
        data.decorator.addEntry(Block::GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::BLUE_ORCHID, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::BROWN_MUSHROOM, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 15.f);
    }

    // SAVANNA
    {
        BIOME_INIT(SAVANNA, "savanna");
        data.tier = BiomeTier::LOWLAND;
        data.climate = { .temperature = 0.45f, .humidity = -0.1f };
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
        BIOME_INIT(ICE_FIELDS, "ice fields");
        data.tier = BiomeTier::LOWLAND;
        data.climate = { .temperature = -0.6f, .humidity = -0.5f };
        data.grassTint = glmUtil::colorFromHex("#8ab4a0");
        data.topBlocks = {
            .top = Block::SNOW,
            .mid = Block::ICE,
        };
    }

    // ==================================================
    // HIGHLAND
    // ==================================================

    // MOUNTAINS
    {
        BIOME_INIT(MOUNTAINS, "mountains");
        data.tier = BiomeTier::HIGHLAND;
        data.climate = { .temperature = -0.4f, .humidity = -0.4f };
        data.grassTint = glmUtil::colorFromHex("#6da36b");
        data.topBlocks = {
            .top = Block::STONE,
            .mid = Block::STONE,
        };
    }

    {
        BIOME_INIT(MESA, "mesa");
        data.grassTint = glmUtil::colorFromHex("#bba357");
        // AIR means retain the height-dependent strata in the surface pass.
        data.topBlocks = { .top = Block::AIR, .mid = Block::AIR };
        data.decorator.addEntry(Block::DEAD_BUSH, 1.f, { Block::TERRACOTTA, Block::ORANGE_TERRACOTTA });
        data.decorator.addEntry(Block::AIR, 80.f);
    }
    {
        BIOME_INIT(TIANZI_MOUNTAINS, "tianzi mountains");
        data.grassTint = glmUtil::colorFromHex("#659749");
        // Cliff trees can root on a narrow step with rock behind their foliage.
        // Require a clear trunk, rather than a full ring of air at ground level.
        data.structureGens = {
            { { { StructureType::PINE_TREE, 5.f,
                    { .height = 10, .clearanceRadius = 0, .supportRadius = 1, .minSupportBlocks = 5,
                      .spacingXZ = 4.5f, .spacingY = 12.f } },
                { StructureType::PINE_SHRUB, 2.f,
                    { .height = 7, .clearanceRadius = 0, .supportRadius = 1, .minSupportBlocks = 3,
                      .spacingXZ = 3.f, .spacingY = 7.f } } }, 8, 2 },
        };
        std::vector<Block> pineGroundBlocks{ Block::GRASS_BLOCK, Block::STONE, FormationRock::tianziPatchBlock };
        pineGroundBlocks.insert(pineGroundBlocks.end(), FormationRock::tianziLayerBlocks.begin(),
                                FormationRock::tianziLayerBlocks.end());
        data.structureGens.back().surfacePlacement = StructureSurfacePlacement{ std::move(pineGroundBlocks) };
        data.decorator.addEntry(Block::GRASS, 3.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 22.f);
    }
    {
        BIOME_INIT(RED_DESERT, "red desert");
        data.grassTint = glmUtil::colorFromHex("#bd9a47");
        data.topBlocks = { .top = Block::RED_SAND, .mid = Block::RED_SANDSTONE };
        data.structureGens = { { StructureType::SAGUARO_CACTUS, 30, 6 }, { StructureType::PALM_TREE, 80, 16 } };
        data.decorator.addEntry(Block::DEAD_BUSH, 2.f, { Block::RED_SAND });
        data.decorator.addEntry(Block::TINY_CACTUS, 1.f, { Block::RED_SAND });
        data.decorator.addEntry(Block::AIR, 65.f);
    }
    {
        BIOME_INIT(OASIS, "oasis");
        data.grassTint = glmUtil::colorFromHex("#72b84c");
        data.topBlocks = { .underwaterTop = Block::SAND, .shoreTop = Block::SAND };
        data.structureGens = { { StructureType::PALM_TREE, 18, 6 } };
        data.decorator.addEntry(Block::GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 9.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::BLUE_ORCHID, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::PINK_DAFFODIL, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 18.f);
    }

    for (auto& candidates : candidatesByTier)
    {
        candidates.clear();
    }
    for (size_t biomeIdx = 0; biomeIdx < biomeDatas.size(); ++biomeIdx)
    {
        candidatesByTier[static_cast<size_t>(biomeDatas[biomeIdx].tier)].push_back(static_cast<Biome>(biomeIdx));
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

    BiomeTier tier;
    if (biomeNoise.inland < -0.15f)
    {
        tier = BiomeTier::OCEAN;
    }
    else if (biomeNoise.inland < 0.0f)
    {
        tier = BiomeTier::BEACH;
    }
    else if (BiomeNoiseFields::isHighland(biomeNoise))
    {
        tier = BiomeTier::HIGHLAND;
    }
    else
    {
        tier = BiomeTier::LOWLAND;
    }

    // Climate alone picks within a tier: relief already chose the tier, and matching on relief
    // too would split neighboring climate targets along relief contours.
    Biome closestBiome = Biome::COUNT;
    float closestDist2 = std::numeric_limits<float>::max();

    for (const Biome biome : candidatesByTier[static_cast<size_t>(tier)])
    {
        const ClimateTarget& target = BIOME_DATA(biome).climate;
        const float dTemperature = biomeNoise.temperature - target.temperature;
        const float dHumidity = biomeNoise.humidity - target.humidity;
        const float dist2 = dTemperature * dTemperature + dHumidity * dHumidity;

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
