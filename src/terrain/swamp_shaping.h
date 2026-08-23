// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome_noise.h"

#include <utility>
#include <vector>

#include <glm/glm.hpp>

// Swamps are cellular ponds contained by dam bands along cell borders. Design rationale and
// invariants: knowledge/terrain/swamp_generation.md.
namespace SwampShaping
{

// Domain-warp amplitudes for cell lookups, applied by the caller before computeShaping; their sum
// must stay well under the cell padding so a warped column still finds its true nearest sites in
// the scan window.
inline constexpr float swampWarpAmplitude = 18.f;
inline constexpr float swampWarpFineAmplitude = 6.f;

struct CellInfo
{
    bool swampy;
    int pondLevel;
};

struct Shaping
{
    float baseHeight;
    float surfaceMultiplier;
    int waterLevel;
    int caveSeal;
};

// Per-chunk scratch: cell sites precomputed for every cell any of the chunk's columns can scan,
// plus lazily computed per-cell info (a chunk touches only a handful of unique swamp cells, and
// each cell's info costs several single-point noise samples).
struct ChunkContext
{
    glm::ivec2 siteGridMinCornerXZ_WS;
    glm::ivec2 siteGridNumCells;
    std::vector<glm::ivec2> sites; // x-innermost
    std::vector<std::pair<glm::ivec2, CellInfo>> cellCache;
};

void init(uint32_t worldSeed);

ChunkContext makeChunkContext(glm::ivec2 chunkPosBlocksXZ_WS, int chunkSizeBlocksXZ);

// Computes swamp pond/dam shaping for one column: the nearest cell site decides its pond, and
// every other scanned site contributes a dam barrier profile. Design rationale and
// window-stability rules: knowledge/terrain/swamp_generation.md.
Shaping computeShaping(glm::vec2 warpedPosXZ_WS,
                       const BiomeNoise& biomeNoise,
                       const BiomeNoiseFields::NaturalTerrain& naturalTerrain,
                       ChunkContext& context);

} // namespace SwampShaping
