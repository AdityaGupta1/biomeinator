/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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

    BLOCK_DATA_BY_NAME(BEDROCK) = { BlockUvs(uvec2(5, 0)) };
    BLOCK_DATA_BY_NAME(STONE) = { BlockUvs(uvec2(0, 0)) };
    BLOCK_DATA_BY_NAME(LAMP) = { .uvs = BlockUvs(uvec2(1, 0)), .emitsLight = true };
    BLOCK_DATA_BY_NAME(DIRT) = { BlockUvs(uvec2(4, 0)) };
    BLOCK_DATA_BY_NAME(GRASS_BLOCK) = { BlockUvs(uvec2(2, 0), uvec2(3, 0), uvec2(4, 0)) };
    BLOCK_DATA_BY_NAME(SAND) = { BlockUvs(uvec2(6, 0)) };
    BLOCK_DATA_BY_NAME(SANDSTONE) = { BlockUvs(uvec2(8, 0), uvec2(7, 0), uvec2(8, 0)) };
    BLOCK_DATA_BY_NAME(SNOW) = { BlockUvs(uvec2(9, 0)) };
    BLOCK_DATA_BY_NAME(SNOWY_GRASS_BLOCK) = { BlockUvs(uvec2(9, 0), uvec2(10, 0), uvec2(4, 0)) };
    BLOCK_DATA_BY_NAME(ICE) = { BlockUvs(uvec2(11, 0)) };
    BLOCK_DATA_BY_NAME(OAK_LOG) = { BlockUvs(uvec2(13, 0), uvec2(12, 0), uvec2(13, 0)) };
    BLOCK_DATA_BY_NAME(OAK_LEAVES) = { .uvs = BlockUvs(uvec2(14, 0)), .type = BlockType::TRANSPARENT_CUTOUT };
    BLOCK_DATA_BY_NAME(CACTUS) = { BlockUvs(uvec2(15, 0)) };
    BLOCK_DATA_BY_NAME(GRASS) = { .uvs = BlockUvs(uvec2(16, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(SHORT_GRASS) = { .uvs = BlockUvs(uvec2(20, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(DEAD_BUSH) = { .uvs = BlockUvs(uvec2(17, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(DEAD_GRASS_1) = { .uvs = BlockUvs(uvec2(18, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(DEAD_GRASS_2) = { .uvs = BlockUvs(uvec2(19, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(GOLDENROD) = { .uvs = BlockUvs(uvec2(21, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(TINY_CACTUS) = { .uvs = BlockUvs(uvec2(22, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
    BLOCK_DATA_BY_NAME(PINK_DAFFODIL) = { .uvs = BlockUvs(uvec2(23, 0)), .type = BlockType::TRANSPARENT_CUTOUT, .shape = BlockShape::X_SHAPED };
}

const BlockData& getBlockData(Block block)
{
    return BLOCK_DATA(block);
}

} // namespace Blocks
