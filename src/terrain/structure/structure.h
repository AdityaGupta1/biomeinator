// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"

#include <glm/glm.hpp>
#include <vector>

enum StructureType
{
    OAK_TREE,
    SAGUARO_CACTUS,
    PALM_TREE,
    ACACIA_TREE,

    COUNT
};

struct Structure
{
    StructureType type;
    glm::ivec3 pos_WS;
};

inline constexpr uint32_t structureMaxChunkRadius = 1;

struct StructureBounds
{
    glm::ivec2 minDiffXZ;
    glm::ivec2 maxDiffXZ;

    StructureBounds() = default;
    StructureBounds(int diff);
    StructureBounds(glm::ivec2 minDiffXZ, glm::ivec2 maxDiffXZ);
};

#define STRUCTURE_GEN_FLAG_ALLOW_UNDERWATER (1 << 0)

struct StructureGen
{
    StructureType type;
    uint32_t gridCellSideLength;
    // Inset on the cell's high edge; guarantees gridCellPadding empty blocks
    // between candidates in adjacent cells.
    uint32_t gridCellPadding{ 0 };
    uint32_t flags{ 0 };
};

namespace Structures
{

void init();

const StructureBounds& getStructureBounds(StructureType type);

} // namespace Structures
