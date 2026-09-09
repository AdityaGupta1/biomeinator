// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block.h"
#include "structure/cave_structure.h"
#include "structure/decorator.h"

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
    CRYSTALS,

    COUNT
};

// One air pocket in a single column, captured during the terrain block-fill scan. Scratch
// only; cave-air biome ownership is persisted separately for the decorator pass.
// start = floor solid y (first air is start + 1); end = top air y (ceiling solid is end + 1);
// layerHeight = end - start = number of air blocks. closed is false when the pocket opens
// upward into non-cave air (no ceiling solid), so ceiling gens are skipped.
// bottomBiome/topBiome are the cave biomes of the floor/ceiling solids.
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
    // Optional second rock type chosen by a low-frequency 3D field, with its own fringe block.
    // AIR disables.
    Block secondaryBaseBlock{ Block::AIR };
    Block secondarySkinFringeBlock{ Block::AIR };
    // Optional replacement for near-surface rock where the cave surface is flat-ish (floors,
    // ceilings, gentle slopes); steep walls and deeper rock keep the base block. Lets a rock with
    // a distinctive top face (columnar basalt) show it only on ledges. AIR disables.
    // See knowledge/terrain/cave_biome_system.md.
    Block flatSurfaceBlock{ Block::AIR };
    Block secondaryFlatSurfaceBlock{ Block::AIR };
    // Optional skin on cave surfaces (floors, walls and ceilings alike): solid voxels whose cave
    // carve noise sits just above the carve threshold become skinBlock, with skinPatchBlock
    // blobs mixed in. skinFringeBlock forms a band just outside the skin, so it shows on the
    // surface where the skin thins out to bare rock; it is only applied to voxels with air
    // directly above (it reads as a top surface). AIR disables each.
    // See knowledge/terrain/cave_biome_system.md.
    Block skinBlock{ Block::AIR };
    Block skinPatchBlock{ Block::AIR };
    Block skinFringeBlock{ Block::AIR };
    // Random LAMP blocks scattered through the biome's rock
    bool scatterLamps{ true };
    std::vector<CaveStructureGen> caveStructureGens{};
    // Applied to permitted floor, wall, and ceiling surfaces bordering this biome's cave air.
    Decorator decorator{};
};

namespace CaveBiomes
{

void init();

const CaveBiomeData& getCaveBiomeData(CaveBiome caveBiome);

// Cave-floor ground blocks that cave flora may stand on
bool isCaveFloraGroundBlock(Block block);

CaveBiome getClosestCaveBiome(const CaveBiomeNoise& caveBiomeNoise);

} // namespace CaveBiomes
