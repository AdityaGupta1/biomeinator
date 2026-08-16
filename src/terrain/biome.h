// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block.h"
#include "structure/decorator.h"
#include "structure/structure.h"

#include <vector>

class RandomNumberGenerator;

struct BiomeNoise
{
    float temperature{ 0.f };
    float humidity{ 0.f };
    float peak{ 0.f };
    float inland{ 0.f };

    float distance2(const BiomeNoise& other) const;

    static BiomeNoise randomOffset(const BiomeNoise& base, RandomNumberGenerator& rng);
};

// Serialized by value in world exports — only append new biomes.
enum class Biome : uint8_t
{
    OCEAN,

    BEACH,
    GRAVEL_BEACH,
    BLACK_SAND_BEACH,

    PLAINS,
    DESERT,
    FOREST,
    TUNDRA,

    SAVANNA,
    ICE_FIELDS,
    MOUNTAINS,

    SWAMP,

    COUNT
};

struct TopBlocks
{
    Block top{ Block::GRASS_BLOCK };
    Block mid{ Block::DIRT };
};

struct BiomeData
{
    const char* name{ "" };
    BiomeNoise biomeNoise{};
    TopBlocks topBlocks{};
    glm::vec3 grassTint{ 1.f, 1.f, 1.f }; // sRGB
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
