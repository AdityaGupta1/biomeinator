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
    : top(top), side(side), bottom(bottom)
{}

namespace Blocks
{

std::array<BlockData, static_cast<size_t>(Block::COUNT)> blockDatas;

#define BLOCK_DATA(block) blockDatas[static_cast<size_t>(block)]
#define BLOCK_DATA_BY_NAME(blockName) blockDatas[static_cast<size_t>(Block::blockName)]

void init()
{
    BLOCK_DATA_BY_NAME(STONE) = { BlockUvs(uvec2(0, 0)) };
    BLOCK_DATA_BY_NAME(LAMP) = { BlockUvs(uvec2(1, 0)), true };
    BLOCK_DATA_BY_NAME(DIRT) = { BlockUvs(uvec2(4, 0)) };
    BLOCK_DATA_BY_NAME(GRASS) = { BlockUvs(uvec2(2, 0), uvec2(3, 0), uvec2(4, 0)) };
}

const BlockData& getBlockData(Block block)
{
    return BLOCK_DATA(block);
}

} // namespace Blocks
