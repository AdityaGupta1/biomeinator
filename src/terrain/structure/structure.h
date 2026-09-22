// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"
#include "util/rng.h"

#include <glm/glm.hpp>
#include <optional>
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
    CYPRESS_TREE,

    PINE_TREE,
    PINE_SHRUB,

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

// Opt-in exposed-surface placement uses a conservative clear envelope around the
// trunk, a supported footprint, and an exclusion ellipsoid around each anchor.
// These are placement requirements, independent of how a structure draws itself.
struct StructureSurfaceFit
{
    uint32_t height{ 1 };
    uint32_t clearanceRadius{ 0 };
    uint32_t supportRadius{ 0 };
    uint32_t minSupportBlocks{ 1 };
    float spacingXZ{ 4.f };
    float spacingY{ 6.f };
};

struct StructureGenVariant
{
    StructureType type;
    float weight{ 1.f };
    StructureSurfaceFit surfaceFit{};
};

struct StructureSurfacePlacement
{
    std::vector<Block> groundBlocks{ Block::GRASS_BLOCK };
};

struct StructureGen
{
    // Weighted list of types sharing this gen's grid; one is rolled per accepted candidate, so
    // all variants inherit the grid's spacing guarantee.
    std::vector<StructureGenVariant> variants;
    uint32_t gridCellSideLength;
    // Inset on the cell's high edge; guarantees gridCellPadding empty blocks
    // between candidates in adjacent cells.
    uint32_t gridCellPadding;
    uint32_t flags;
    // Unset: the ordinary one-candidate-per-XZ-cell ground grid. Set: inspect
    // actual exposed surfaces, including lower ledges, and fit/thin them in 3D.
    std::optional<StructureSurfacePlacement> surfacePlacement{};

    StructureGen(StructureType type, uint32_t gridCellSideLength, uint32_t gridCellPadding = 0, uint32_t flags = 0);
    StructureGen(std::vector<StructureGenVariant> variants,
                 uint32_t gridCellSideLength,
                 uint32_t gridCellPadding = 0,
                 uint32_t flags = 0);

    StructureType pickVariant(RandomNumberGenerator& rng) const;
    // Distinguishes this gen's candidate grid from other gens over the same cells.
    uint32_t gridSalt() const;
};

// Published with terrain, immutable thereafter. Neighboring chunks independently
// resolve these against immutable terrain masks before filling the same geometry.
struct SurfaceStructureCandidate
{
    glm::ivec3 pos_WS;
    const StructureGen* gen;
    uint32_t priority;
    uint32_t headroom;
};

namespace Structures
{

void init();

const StructureBounds& getStructureBounds(StructureType type);

} // namespace Structures
