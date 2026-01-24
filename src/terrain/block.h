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

#pragma once

#include <cstdint>
#include <glm/glm.hpp>

using BlockId = uint16_t;

enum class Block : BlockId
{
	AIR = 0,
	STONE,
	LAMP,

	COUNT
};

struct BlockUvs
{
    glm::uvec2 top{}; // integer position in block texture grid
    glm::uvec2 side{};
    glm::uvec2 bottom{};

    BlockUvs() = default;
    BlockUvs(glm::uvec2 all);
    BlockUvs(glm::uvec2 top, glm::uvec2 side, glm::uvec2 bottom);
};

struct BlockData
{
    BlockUvs uvs;
    bool emitsLight{ false };
};

namespace Blocks
{

void init();

const BlockData& getBlockData(Block block);

} // namespace Blocks
