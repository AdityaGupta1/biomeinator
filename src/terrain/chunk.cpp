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

#include "chunk.h"

#include "noise.h"

using namespace glm;

Chunk::Chunk(ivec2 chunkPos)
	: chunkPos(chunkPos)
{}

void Chunk::generateBlocks()
{
    const ivec2 chunkBlockPos_WS = chunkPos * 16;

    for (uint y = 0; y < 16; ++y)
    {
        for (uint x = 0; x < 16; ++x)
        {
            const ivec2 blockPosXY_WS = chunkBlockPos_WS + ivec2(x, y);
            const uint height = uint(64.f + 10.f * (sinf(blockPosXY_WS.x * 0.1f) * cosf(blockPosXY_WS.y * 0.1f)));

            for (uint z = 0; z < height; ++z)
            {
                const ivec3 blockPos_CS = ivec3(x, y, z);
                this->blocks[blockPosToIdx(blockPos_CS)] = Block::STONE;
            }

            if (rand1(blockPosXY_WS) < 0.05f)
            {
                this->blocks[blockPosToIdx(ivec3(x, y, height))] = Block::LAMP;
            }
        }
    }
}

void Chunk::createInstance(Scene* scene)
{

}

uint32_t Chunk::blockPosToIdx(glm::uvec3 blockPos)
{
    return blockPos.z
		 + blockPos.x * CHUNK_SIZE_Z
		 + blockPos.y * CHUNK_SIZE_X * CHUNK_SIZE_Z;
}
