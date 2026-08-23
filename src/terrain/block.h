// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block_ids.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>

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

struct BlockUvs
{
private:
    glm::uvec2 uvs[3]; // integer position in block texture grid
                       // order = side, top, bottom

public:
    BlockUvs() = default;
    BlockUvs(glm::uvec2 all);
    BlockUvs(glm::uvec2 top, glm::uvec2 side, glm::uvec2 bottom);

    const glm::uvec2& operator[](uint32_t idx) const;
};

struct BlockData
{
    BlockUvs uvs{};
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

} // namespace Blocks
