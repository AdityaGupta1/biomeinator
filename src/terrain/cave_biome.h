// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block.h"
#include "structure/cave_structure.h"

#include <vector>

struct CaveBiomeNoise
{
    float temperature{ 0.f };
    float humidity{ 0.f };

    float distance2(const CaveBiomeNoise& other) const;
};

enum class CaveBiome : uint8_t
{
    STONE,

    LUSH,
    BRIMSTONE,
    MARBLE_CRYSTALS,
    SUBMARINE,

    COUNT
};

// One air pocket in a single column, captured during the terrain block-fill scan. Scratch
// only — never persisted. start = floor solid y (first air is start + 1); end = top air y
// (ceiling solid is end + 1); layerHeight = end - start = number of air blocks. closed is
// false when the pocket opens upward into non-cave air (no ceiling solid), so ceiling gens
// are skipped. bottomBiome/topBiome are the cave biomes of the floor/ceiling solids.
struct CaveLayer
{
    int start;
    int end;
    CaveBiome bottomBiome;
    CaveBiome topBiome;
    bool closed;
};

struct CaveBiomeData
{
    CaveBiomeNoise biomeNoise{};
    Block baseBlock{ Block::STONE };
    std::vector<CaveStructureGen> caveStructureGens{};
};

namespace CaveBiomes
{

void init();

const CaveBiomeData& getCaveBiomeData(CaveBiome caveBiome);

CaveBiome getClosestCaveBiome(const CaveBiomeNoise& caveBiomeNoise);

} // namespace CaveBiomes
