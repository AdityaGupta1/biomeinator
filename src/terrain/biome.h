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

#pragma once

#include "block.h"
#include "structure/decorator.h"
#include "structure/structure.h"

#include <vector>

struct BiomeNoise
{
    float temperature{ 0.f };
    float humidity{ 0.f };
    float peak{ 0.f };

    float distance2(const BiomeNoise& other) const;
};

enum class Biome : uint8_t
{
    PLAINS,
    SAVANNA,
    DESERT,
    FOREST,
    MOUNTAINS,
    TUNDRA,
    ICE_FIELDS,

    COUNT
};

struct TopBlocks
{
    Block top{ Block::GRASS_BLOCK };
    Block mid{ Block::DIRT };
};

struct BiomeData
{
    BiomeNoise biomeNoise{};
    TopBlocks topBlocks{};
    std::vector<StructureGen> structureGens{};
    Decorator decorator{};
};

struct BiomeWeight
{
    Biome biome;
    float weight;
};

namespace Biomes
{

void init();

const BiomeData& getBiomeData(Biome biome);

Biome getClosestBiome(const BiomeNoise& biomeNoise);

} // namespace Biomes
