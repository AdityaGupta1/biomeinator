// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "block.h"

#include <array>

using namespace glm;

BlockUvs::BlockUvs(uvec2 all)
    : BlockUvs(all, all, all)
{}

BlockUvs::BlockUvs(uvec2 top, uvec2 side, uvec2 bottom)
    : uvs{ side, top, bottom }
{}

const glm::uvec2& BlockUvs::operator[](uint32_t idx) const
{
    return this->uvs[idx];
}

namespace Blocks
{

std::array<BlockData, static_cast<size_t>(Block::COUNT)> blockDatas;

#define BLOCK_DATA(block) blockDatas[static_cast<size_t>(block)]
#define BLOCK_DATA_BY_NAME(blockName) blockDatas[static_cast<size_t>(Block::blockName)]

void init()
{
    BLOCK_DATA_BY_NAME(AIR) = { .type = BlockType::AIR };

    BLOCK_DATA_BY_NAME(WATER) = { .type = BlockType::WATER, .shape = BlockShape::CUBE };
    BLOCK_DATA_BY_NAME(WATER_TOP) = { .type = BlockType::WATER, .shape = BlockShape::LIQUID_TOP };

    BLOCK_DATA_BY_NAME(BEDROCK) = { .uvs = { uvec2(5, 0) } };
    BLOCK_DATA_BY_NAME(STONE) = { .uvs = { uvec2(0, 0) } };
    BLOCK_DATA_BY_NAME(LAMP) = { .uvs = { uvec2(1, 0) }, .emitsLight = true };
    BLOCK_DATA_BY_NAME(DIRT) = { .uvs = { uvec2(4, 0) } };
    BLOCK_DATA_BY_NAME(GRASS_BLOCK) = { .uvs = { uvec2(2, 0), uvec2(3, 0), uvec2(4, 0) } };
    BLOCK_DATA_BY_NAME(SAND) = { .uvs = { uvec2(6, 0) } };
    BLOCK_DATA_BY_NAME(SANDSTONE) = { .uvs = { uvec2(8, 0), uvec2(7, 0), uvec2(8, 0) } };
    BLOCK_DATA_BY_NAME(SNOW) = { .uvs = { uvec2(9, 0) } };
    BLOCK_DATA_BY_NAME(SNOWY_GRASS_BLOCK) = { .uvs = { uvec2(9, 0), uvec2(10, 0), uvec2(4, 0) } };
    BLOCK_DATA_BY_NAME(ICE) = { .uvs = { uvec2(11, 0) } };
    BLOCK_DATA_BY_NAME(OAK_LOG) = { .uvs = { uvec2(13, 0), uvec2(12, 0), uvec2(13, 0) } };
    BLOCK_DATA_BY_NAME(OAK_LEAVES) = { .uvs = { uvec2(14, 0) }, .type = BlockType::TRANSPARENT_CUTOUT };
    BLOCK_DATA_BY_NAME(CACTUS) = { .uvs = { uvec2(15, 0) } };
    BLOCK_DATA_BY_NAME(GRASS) = { .uvs = { uvec2(16, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(SHORT_GRASS) = { .uvs = { uvec2(20, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(DEAD_BUSH) = { .uvs = { uvec2(17, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(DEAD_GRASS_1) = { .uvs = { uvec2(18, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(DEAD_GRASS_2) = { .uvs = { uvec2(19, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(GOLDENROD) = { .uvs = { uvec2(21, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(TINY_CACTUS) = { .uvs = { uvec2(22, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(PINK_DAFFODIL) = { .uvs = { uvec2(23, 0) }, .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(BLACK_SAND) = { .uvs = { uvec2(24, 0) } };
    BLOCK_DATA_BY_NAME(GRAVEL) = { .uvs = { uvec2(25, 0) } };
    BLOCK_DATA_BY_NAME(PALM_LOG) = { .uvs = { uvec2(27, 0), uvec2(26, 0), uvec2(27, 0) } };
    BLOCK_DATA_BY_NAME(PALM_LEAVES) = { .uvs = { uvec2(28, 0) }, .type = BlockType::TRANSPARENT_CUTOUT };
    BLOCK_DATA_BY_NAME(ACACIA_LOG) = { .uvs = { uvec2(30, 0), uvec2(29, 0), uvec2(30, 0) } };
    BLOCK_DATA_BY_NAME(ACACIA_LEAVES) = { .uvs = { uvec2(31, 0) }, .type = BlockType::TRANSPARENT_CUTOUT };
}

const BlockData& getBlockData(Block block)
{
    return BLOCK_DATA(block);
}

} // namespace Blocks
