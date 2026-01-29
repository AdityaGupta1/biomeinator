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

#include "../chunk.h"

#include <array>
#include <limits>

#include <glm/glm.hpp>

float BiomeNoise::distance2(const BiomeNoise& other) const
{
    return (this->temperature - other.temperature) * (this->temperature - other.temperature) +
           (this->humidity - other.humidity) * (this->humidity - other.humidity);
}

namespace Biomes
{

static std::array<BiomeData, static_cast<size_t>(Biome::COUNT)> biomeDatas;

#define BIOME_DATA(biome) biomeDatas[static_cast<size_t>(biome)]
#define BIOME_DATA_BY_NAME(biomeName) biomeDatas[static_cast<size_t>(Biome::biomeName)]

void init()
{
    // PLAINS
    BIOME_DATA_BY_NAME(PLAINS).biomeNoise = {
        .temperature = 0.0f,
        .humidity = 0.0f,
    };

    // SAVANNA
    BIOME_DATA_BY_NAME(SAVANNA).biomeNoise = {
        .temperature = 0.6f,
        .humidity = -0.6f,
    };

    // DESERT
    BIOME_DATA_BY_NAME(DESERT).biomeNoise = {
        .temperature = 1.0f,
        .humidity = -1.0f,
    };
    BIOME_DATA_BY_NAME(DESERT).topBlocks = {
        .top = Block::SAND,
        .mid = Block::SANDSTONE,
    };

    // FOREST
    BIOME_DATA_BY_NAME(FOREST).biomeNoise = {
        .temperature = -0.1f,
        .humidity = 0.2f,
    };

    // MOUNTAINS
    BIOME_DATA_BY_NAME(MOUNTAINS).biomeNoise = {
        .temperature = -0.4f,
        .humidity = -0.4f,
    };
    BIOME_DATA_BY_NAME(MOUNTAINS).topBlocks = {
        .top = Block::STONE,
        .mid = Block::STONE,
    };

    // TUNDRA
    BIOME_DATA_BY_NAME(TUNDRA).biomeNoise = {
        .temperature = -0.7f,
        .humidity = -0.6f,
    };
    BIOME_DATA_BY_NAME(TUNDRA).topBlocks = {
        .top = Block::SNOWY_GRASS,
        .mid = Block::DIRT,
    };

    // ICE_FIELDS
    BIOME_DATA_BY_NAME(ICE_FIELDS).biomeNoise = {
        .temperature = -0.85f,
        .humidity = -0.8f,
    };
    BIOME_DATA_BY_NAME(ICE_FIELDS).topBlocks = {
        .top = Block::SNOW,
        .mid = Block::ICE,
    };
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
