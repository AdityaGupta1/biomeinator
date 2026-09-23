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
    float erosion{ 0.f };

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

    MESA,
    TIANZI_MOUNTAINS,
    RED_DESERT,
    OASIS,

    COUNT
};

struct TopBlocks
{
    Block top{ Block::GRASS_BLOCK };
    Block mid{ Block::DIRT };
    // When set (non-AIR), replaces a grass top block underwater (instead of the global dirt
    // fallback) or in the noise-driven shore band just above water level.
    Block underwaterTop{ Block::AIR };
    Block shoreTop{ Block::AIR };
};

// Which climate search a biome competes in. Relief and coastline pick the tier; climate then picks
// the nearest target within it.
enum class BiomeTier : uint8_t
{
    // Never a climate candidate: chosen by a terrain regime or a spatial feature (oasis).
    NONE,
    OCEAN,
    BEACH,
    LOWLAND,
    HIGHLAND,

    COUNT
};

struct ClimateTarget
{
    float temperature{ 0.f };
    float humidity{ 0.f };
};

struct BiomeData
{
    const char* name{ "" };
    BiomeTier tier{ BiomeTier::NONE };
    ClimateTarget climate{};
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
