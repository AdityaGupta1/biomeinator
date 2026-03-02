/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "biome.h"

#include "chunk.h"

#include <array>
#include <limits>

#include <glm/glm.hpp>

float BiomeNoise::distance2(const BiomeNoise& other) const
{
    return (this->temperature - other.temperature) * (this->temperature - other.temperature) +
           (this->humidity - other.humidity) * (this->humidity - other.humidity) +
           (this->peak - other.peak) * (this->peak - other.peak);
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

// temperature in [-1, 1]
// humidity in [-1, 1]
// peak in [-1, 1]
void init()
{
    // ==================================================
    // OCEAN
    // ==================================================

    // OCEAN
    {
        BiomeData& data = BIOME_DATA_BY_NAME(OCEAN);
        oceanBiomes.push_back(Biome::OCEAN);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = -1.0f,
        };
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
        beachBiomes.push_back(Biome::BEACH);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = -1.0f,
        };
        data.topBlocks = {
            .top = Block::SAND,
            .mid = Block::SAND,
        };
    }

    // ==================================================
    // LOWLAND
    // ==================================================

    // PLAINS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(PLAINS);
        lowlandBiomes.push_back(Biome::PLAINS);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = -0.7f,
        };
        data.decorator.addEntry(Block::GRASS, 5.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::PINK_DAFFODIL, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 15.f);
    }

    // DESERT
    {
        BiomeData& data = BIOME_DATA_BY_NAME(DESERT);
        lowlandBiomes.push_back(Biome::DESERT);
        data.biomeNoise = {
            .temperature = 1.0f,
            .humidity = -1.0f,
            .peak = -0.6f,
        };
        data.topBlocks = {
            .top = Block::SAND,
            .mid = Block::SANDSTONE,
        };
        data.structureGens = {
            { StructureType::SAGUARO_CACTUS, 20, 4.0f },
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
        lowlandBiomes.push_back(Biome::FOREST);
        data.biomeNoise = {
            .temperature = -0.1f,
            .humidity = 0.2f,
            .peak = -0.4f,
        };
        data.structureGens = {
            { StructureType::OAK_TREE, 7, 1.5f },
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
        lowlandBiomes.push_back(Biome::TUNDRA);
        data.biomeNoise = {
            .temperature = -0.7f,
            .humidity = -0.6f,
            .peak = -0.6f,
        };
        data.topBlocks = {
            .top = Block::SNOWY_GRASS_BLOCK,
            .mid = Block::DIRT,
        };
    }

    // ==================================================
    // HIGHLAND
    // ==================================================

    // SAVANNA
    {
        BiomeData& data = BIOME_DATA_BY_NAME(SAVANNA);
        highlandBiomes.push_back(Biome::SAVANNA);
        data.biomeNoise = {
            .temperature = 0.6f,
            .humidity = -0.6f,
            .peak = -0.2f,
        };
        data.decorator.addEntry(Block::GRASS, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 10.f);
    }

    // ICE_FIELDS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(ICE_FIELDS);
        highlandBiomes.push_back(Biome::ICE_FIELDS);
        data.biomeNoise = {
            .temperature = -0.85f,
            .humidity = -0.8f,
            .peak = -0.3f,
        };
        data.topBlocks = {
            .top = Block::SNOW,
            .mid = Block::ICE,
        };
    }

    // MOUNTAINS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(MOUNTAINS);
        highlandBiomes.push_back(Biome::MOUNTAINS);
        data.biomeNoise = {
            .temperature = -0.4f,
            .humidity = -0.4f,
            .peak = 0.6f,
        };
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
    std::vector<Biome>* closestBiomeCandidates;
    if (biomeNoise.inland < -0.1f)
    {
        closestBiomeCandidates = &oceanBiomes;
    }
    else if (biomeNoise.inland < 0.0f)
    {
        closestBiomeCandidates = &beachBiomes;
    }
    else if (biomeNoise.inland < 0.6f)
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
