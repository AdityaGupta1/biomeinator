// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block_ids.h"

#include <cstdint>
#include <filesystem>
#include <array>
#include <string>
#include <string_view>
#include <vector>

static_assert(static_cast<BlockId>(Block::AIR) == 0, "Chunk block storage assumes AIR == 0");

enum class BlockType : uint8_t
{
    AIR,
    WATER,
    SOLID,
    TRANSPARENT_CUTOUT,
    // Opaque-alpha cubes rendered as glass (specular reflection + refraction); see
    // knowledge/terrain/block_system.md
    GLASS,

    COUNT
};

enum class BlockShape : uint8_t
{
    CUBE,
    X_SHAPED,
    LIQUID_TOP,
    DECORATOR_CUSTOM,

    COUNT
};

// Slice value for untextured blocks (air, water); never sampled
inline constexpr uint32_t TEX_SLICE_INVALID = ~0u;

struct BlockTexSlices
{
private:
    // Texture array slice per face; order = side, top, bottom
    uint32_t slices[3]{ TEX_SLICE_INVALID, TEX_SLICE_INVALID, TEX_SLICE_INVALID };

public:
    BlockTexSlices() = default;
    explicit BlockTexSlices(uint32_t all);
    BlockTexSlices(uint32_t top, uint32_t side, uint32_t bottom);

    uint32_t operator[](uint32_t idx) const;
};

struct BlockData
{
    BlockTexSlices texSlices{};
    BlockType type{ BlockType::SOLID };
    BlockShape shape{ BlockShape::CUBE };
    bool emitsLight{ false };
    bool translucent{ false }; // thin diffuse transmission (leaves and living foliage)
    // Color comes from a world-space ramp rather than the block's texture (see getProceduralColor)
    bool proceduralColor{ false };
    uint32_t modelIdx{ ~0u };
    std::array<uint8_t, 4> rotationY{ 0, 0, 0, 0 }; // quarter turns
    uint8_t numRotationsY{ 1 };
};

// These shapes never hide a neighboring solid or cutout cube face.
constexpr bool isDecoratorShape(BlockShape shape)
{
    return shape == BlockShape::X_SHAPED || shape == BlockShape::DECORATOR_CUSTOM;
}

// Face direction ordering matches chunk neighbor directions: +X,+Z,-X,-Z,+Y,-Y.
// Called after neighbor lookup; world-height boundaries are handled by the chunk.
constexpr bool blockFaceVisible(BlockType type, BlockShape shape, BlockType neighborType,
                                BlockShape neighborShape, int faceIdx)
{
    if (neighborType == BlockType::AIR) return true;
    if ((type == BlockType::SOLID || type == BlockType::TRANSPARENT_CUTOUT) &&
        isDecoratorShape(neighborShape)) return true;
    switch (type)
    {
    case BlockType::SOLID:
        if (neighborType != BlockType::SOLID) return true;
        if (faceIdx == 4) return shape == BlockShape::LIQUID_TOP;
        if (faceIdx == 5) return neighborShape == BlockShape::LIQUID_TOP;
        return neighborShape == BlockShape::LIQUID_TOP && shape != BlockShape::LIQUID_TOP;
    case BlockType::TRANSPARENT_CUTOUT:
        if (neighborType == BlockType::SOLID) return false;
        // Only the lower-positioned cube owns a shared cutout boundary.
        return neighborType != BlockType::TRANSPARENT_CUTOUT || neighborShape != BlockShape::CUBE ||
               faceIdx == 0 || faceIdx == 1 || faceIdx == 4;
    case BlockType::GLASS:
        return neighborType != BlockType::GLASS && neighborType != BlockType::SOLID;
    case BlockType::WATER:
        return shape == BlockShape::LIQUID_TOP && faceIdx == 4;
    default:
        return false;
    }
}

namespace Blocks
{

void init();

// Malformed custom definitions throw; other failures log and return defaults.
BlockData readBlockJson(const std::filesystem::path& jsonPath);

const BlockData& getBlockData(Block block);

// Returns Block::COUNT if no block has the given id.
Block fromId(std::string_view id);

// Distinct texture names referenced by the block definitions; index = texture array slice
const std::vector<std::string>& getTextureNames();

} // namespace Blocks
