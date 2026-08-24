// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block_ids.h"

#include <cstdint>
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

    COUNT
};

enum class BlockShape : uint8_t
{
    CUBE,
    X_SHAPED,
    LIQUID_TOP,

    COUNT
};

// Slice value for blocks with no textures (air, water); their materials have no textures so
// the slice is never sampled, and out-of-range lookups (biome tint, OMM cutout) return false
inline constexpr uint32_t TEX_SLICE_INVALID = ~0u;

struct BlockTexSlices
{
private:
    uint32_t slices[3]{ TEX_SLICE_INVALID, TEX_SLICE_INVALID, TEX_SLICE_INVALID };
    // texture array slice per face
    // order = side, top, bottom

public:
    BlockTexSlices() = default;
    BlockTexSlices(uint32_t all);
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
};

namespace Blocks
{

void init();

const BlockData& getBlockData(Block block);

// Returns Block::COUNT if no block has the given id.
Block fromId(std::string_view id);

// Distinct texture names referenced by the block definitions; index = texture array slice
const std::vector<std::string>& getTextureNames();

} // namespace Blocks
