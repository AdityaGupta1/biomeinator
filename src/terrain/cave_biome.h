// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block.h"

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

struct CaveBiomeData
{
    CaveBiomeNoise biomeNoise{};
    Block baseBlock{ Block::STONE };
};

namespace CaveBiomes
{

void init();

const CaveBiomeData& getCaveBiomeData(CaveBiome caveBiome);

CaveBiome getClosestCaveBiome(const CaveBiomeNoise& caveBiomeNoise);

} // namespace CaveBiomes
