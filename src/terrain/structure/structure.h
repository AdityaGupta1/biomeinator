// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"
#include "util/rng.h"

#include <glm/glm.hpp>
#include <vector>

// Serialized by value in world exports — only append new types.
enum class StructureType : uint8_t
{
    OAK_TREE,
    SAGUARO_CACTUS,
    PALM_TREE,
    ACACIA_TREE,
    LARGE_OAK_TREE,
    BIRCH_TREE,

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

struct StructureGenVariant
{
    StructureType type;
    float weight{ 1.f };
};

struct StructureGen
{
    // Weighted list of types sharing this gen's grid; one is rolled per accepted candidate, so
    // all variants inherit the grid's spacing guarantee.
    std::vector<StructureGenVariant> variants{};
    uint32_t gridCellSideLength{ 16 };
    // Inset on the cell's high edge; guarantees gridCellPadding empty blocks
    // between candidates in adjacent cells.
    uint32_t gridCellPadding{ 0 };
    uint32_t flags{ 0 };

    StructureType pickVariant(RandomNumberGenerator& rng) const;
    // Distinguishes this gen's candidate grid from other gens over the same cells.
    uint32_t candidateSalt() const;
};

namespace Structures
{

void init();

const StructureBounds& getStructureBounds(StructureType type);

} // namespace Structures
