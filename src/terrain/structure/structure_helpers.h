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

#include "../block.h"
#include "../chunk.h"

namespace StructureHelpers
{

inline void tryPlaceStructureBlock(std::vector<Block>& blocks, uint32_t blockIdx, Block newBlock)
{
    Block& block = blocks[blockIdx];
    if (block == Block::AIR || block == Block::WATER || block == Block::WATER_TOP)
    {
        block = newBlock;
    }
}

void fillLine(std::vector<Block>& blocks, glm::ivec3 startPos_CS, glm::ivec3 endPos_CS, Block block)
{
    const glm::ivec3 d = glm::abs(endPos_CS - startPos_CS);
    const glm::ivec3 s = glm::sign(endPos_CS - startPos_CS);
    glm::ivec3 pos = startPos_CS;

    // Determine dominant axis
    const int axis0 = (d.x >= d.y && d.x >= d.z) ? 0 : (d.y >= d.z) ? 1 : 2;
    const int axis1 = (axis0 + 1) % 3;
    const int axis2 = (axis0 + 2) % 3;

    const int dm = d[axis0];
    int err1 = 2 * d[axis1] - dm;
    int err2 = 2 * d[axis2] - dm;
    const int dErr1 = 2 * d[axis1];
    const int dErr2 = 2 * d[axis2];
    const int dm2 = 2 * dm;

    for (int i = 0; i <= dm; ++i)
    {
        if (Chunk::isInChunk(pos))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(glm::uvec3(pos)), block);
        }

        if (err1 > 0) { pos[axis1] += s[axis1]; err1 -= dm2; }
        if (err2 > 0) { pos[axis2] += s[axis2]; err2 -= dm2; }
        err1 += dErr1;
        err2 += dErr2;
        pos[axis0] += s[axis0];
    }
}

} // namespace StructureHelpers
