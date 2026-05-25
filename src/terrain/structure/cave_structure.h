// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"

#include <glm/glm.hpp>
#include <vector>

enum class CaveStructureType : uint8_t
{
    CRYSTAL,
    HANGING_LAMP,
    STONE_COLUMN,

    COUNT
};

struct CaveStructure
{
    CaveStructureType type;
    glm::ivec3 pos_WS;
    // Air blocks available in the host pocket (end - start). STONE_COLUMN fills this
    // many blocks; the fixed-height structures ignore it (clearance guaranteed by minLayerHeight).
    int availableHeight;
};

struct CaveStructureBounds
{
    glm::ivec2 minDiffXZ;
    glm::ivec2 maxDiffXZ;

    CaveStructureBounds() = default;
    CaveStructureBounds(int diff);
    CaveStructureBounds(glm::ivec2 minDiffXZ, glm::ivec2 maxDiffXZ);
};

#define CAVE_STRUCTURE_GEN_FLAG_ALLOW_LAVA (1 << 0)

struct CaveStructureGen
{
    CaveStructureType type;
    // Floor gens (false) anchor at the pocket floor; ceiling gens (true) anchor at the
    // ceiling and are only tried on closed pockets.
    bool generatesFromCeiling{ false };
    uint32_t minLayerHeight{ 0 };
    uint32_t gridCellSideLength;
    // Inset on the cell's high edge; guarantees gridCellPadding empty blocks
    // between candidates in adjacent cells. Mirrors StructureGen.
    uint32_t gridCellPadding{ 0 };
    uint32_t flags{ 0 };
};

namespace CaveStructures
{

void init();

const CaveStructureBounds& getCaveStructureBounds(CaveStructureType type);

} // namespace CaveStructures
