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

constexpr BiomeNoise biomeNoiseDistance2Scale = {
    .temperature = 1.f,
    .humidity = 1.f,
    .peak = 1.f,
    .inland = 6.f,
};

float BiomeNoise::distance2(const BiomeNoise& other) const
{
    return (this->temperature - other.temperature) * (this->temperature - other.temperature) * biomeNoiseDistance2Scale.temperature +
           (this->humidity - other.humidity) * (this->humidity - other.humidity) * biomeNoiseDistance2Scale.humidity +
           (this->peak - other.peak) * (this->peak - other.peak) * (4.f * biomeNoiseDistance2Scale.peak) + // 4x due to having half the range of the others
           (this->inland - other.inland) * (this->inland - other.inland) * biomeNoiseDistance2Scale.inland;
}

namespace Biomes
{

static std::array<BiomeData, static_cast<size_t>(Biome::COUNT)> biomeDatas;

#define BIOME_DATA(biome) biomeDatas[static_cast<size_t>(biome)]
#define BIOME_DATA_BY_NAME(biomeName) biomeDatas[static_cast<size_t>(Biome::biomeName)]

// temperature in [-1, 1]
// humidity in [-1, 1]
// peak in [0, 1]
void init()
{
    // OCEAN
    {
        BiomeData& data = BIOME_DATA_BY_NAME(OCEAN);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = 0.0f,
            .inland = -0.2f,
        };
        data.topBlocks = {
            //.top = Block::SAND,
            .top = Block::OAK_LOG, // TODO: temporary
            .mid = Block::SAND,
        };
    }

    // BEACH
    {
        BiomeData& data = BIOME_DATA_BY_NAME(BEACH);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = 0.0f,
            .inland = 0.0f,
        };
        data.topBlocks = {
            .top = Block::SAND,
            .mid = Block::SAND,
        };
    }

    // PLAINS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(PLAINS);
        data.biomeNoise = {
            .temperature = 0.0f,
            .humidity = 0.0f,
            .peak = 0.15f,
            .inland = 0.2f,
        };
        data.decorator.addEntry(Block::GRASS, 5.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 6.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 1.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::PINK_DAFFODIL, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 15.f);
    }

    // SAVANNA
    {
        BiomeData& data = BIOME_DATA_BY_NAME(SAVANNA);
        data.biomeNoise = {
            .temperature = 0.6f,
            .humidity = -0.6f,
            .peak = 0.4f,
            .inland = 0.4f,
        };
        data.decorator.addEntry(Block::GRASS, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::SHORT_GRASS, 8.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::GOLDENROD, 2.f, { Block::GRASS_BLOCK });
        data.decorator.addEntry(Block::AIR, 10.f);
    }

    // DESERT
    {
        BiomeData& data = BIOME_DATA_BY_NAME(DESERT);
        data.biomeNoise = {
            .temperature = 1.0f,
            .humidity = -1.0f,
            .peak = 0.2f,
            .inland = 0.3f,
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
        data.biomeNoise = {
            .temperature = -0.1f,
            .humidity = 0.2f,
            .peak = 0.3f,
            .inland = 0.2f,
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

    // MOUNTAINS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(MOUNTAINS);
        data.biomeNoise = {
            .temperature = -0.4f,
            .humidity = -0.4f,
            .peak = 0.8f,
            .inland = 0.8f,
        };
        data.topBlocks = {
            .top = Block::STONE,
            .mid = Block::STONE,
        };
    }

    // TUNDRA
    {
        BiomeData& data = BIOME_DATA_BY_NAME(TUNDRA);
        data.biomeNoise = {
            .temperature = -0.7f,
            .humidity = -0.6f,
            .peak = 0.2f,
            .inland = 0.3f,
        };
        data.topBlocks = {
            .top = Block::SNOWY_GRASS_BLOCK,
            .mid = Block::DIRT,
        };
    }

    // ICE_FIELDS
    {
        BiomeData& data = BIOME_DATA_BY_NAME(ICE_FIELDS);
        data.biomeNoise = {
            .temperature = -0.85f,
            .humidity = -0.8f,
            .peak = 0.15f,
            .inland = 0.55f,
        };
        data.topBlocks = {
            .top = Block::SNOW,
            .mid = Block::ICE,
        };
    }
}

const BiomeData& getBiomeData(Biome biome)
{
    return BIOME_DATA(biome);
}

Biome getClosestBiome(const BiomeNoise& biomeNoise)
{
    Biome closestBiome = Biome::COUNT;
    float closestDist2 = std::numeric_limits<float>::max();

    for (size_t biomeIdx = 0; biomeIdx < static_cast<size_t>(Biome::COUNT); ++biomeIdx)
    {
        const Biome biome = static_cast<Biome>(biomeIdx);
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
