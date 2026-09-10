// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>

// Ordering matches chunk NeighborDirection for the four horizontal faces.
enum class BlockFace : uint8_t
{
    X_POS,
    Z_POS,
    X_NEG,
    Z_NEG,
    Y_POS,
    Y_NEG,

    COUNT
};

inline constexpr uint8_t blockFaceCount = static_cast<uint8_t>(BlockFace::COUNT);
inline constexpr uint8_t blockStateFaceMask = 0x7u;

struct BlockFaceBasis
{
    glm::ivec3 normal;
    glm::ivec3 tangentX;
    glm::ivec3 tangentZ;
};

inline constexpr std::array<BlockFaceBasis, blockFaceCount> blockFaceBases = {{
    { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } },  // +X
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, -1, 0 } },  // +Z
    { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },  // -X
    { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 } },  // -Z
    { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },   // +Y
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, // -Y
}};

constexpr uint8_t blockFaceIndex(BlockFace face)
{
    return static_cast<uint8_t>(face);
}

constexpr const BlockFaceBasis& blockFaceBasis(BlockFace face)
{
    return blockFaceBases[blockFaceIndex(face)];
}

inline glm::vec3 orientToBlockFace(glm::vec3 v, BlockFace face)
{
    const BlockFaceBasis& basis = blockFaceBasis(face);
    return glm::vec3(basis.tangentX) * v.x + glm::vec3(basis.normal) * v.y +
           glm::vec3(basis.tangentZ) * v.z;
}
