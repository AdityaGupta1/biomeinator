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

#include "chunk_generator.h"

#include "chunk.h"
#include "rng.h"

using namespace glm;

namespace ChunkGenerator
{

void fillBlocks(glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    for (uint z = 0; z < chunkSizeXZ; ++z)
    {
        for (uint x = 0; x < chunkSizeXZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(x, z);
            const uint height = uint(64.f + 10.f * (sinf(blockPosXZ_WS.x * 0.1f) * cosf(blockPosXZ_WS.y * 0.1f)));

            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(x, z));

            for (uint y = 0; y < height; ++y)
            {
                blocks[blockIdx++] = (y == height - 1) ? Block::GRASS : Block::DIRT;
            }

            if (rand1(uvec2(blockPosXZ_WS)) < 0.005f && height < chunkSizeY)
            {
                blocks[blockIdx++] = Block::LAMP;
            }
        }
    }
}

}; // namespace ChunkGenerator
