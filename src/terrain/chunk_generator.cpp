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

#include <array>

#include <FastNoise/FastNoise.h>

using namespace glm;

namespace ChunkGenerator
{

void fillBlocks(glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    auto fnSimplex = FastNoise::New<FastNoise::Simplex>();
    fnSimplex->SetScale(200.f);
    auto fnFractal = FastNoise::New<FastNoise::FractalFBm>();
    fnFractal->SetSource(fnSimplex);
    fnFractal->SetOctaveCount(4);
    auto fnMul = FastNoise::New<FastNoise::Multiply>();
    fnMul->SetLHS(fnFractal);
    fnMul->SetRHS(24.f);
    auto fnAdd = FastNoise::New<FastNoise::Add>();
    fnAdd->SetLHS(fnMul);
    fnAdd->SetRHS(80.f);

    std::array<float, chunkSizeXZ * chunkSizeXZ> heightfield;
    fnAdd->GenUniformGrid2D(
        heightfield.data(), chunkPosBlocksXZ_WS.x, chunkPosBlocksXZ_WS.y, chunkSizeXZ, chunkSizeXZ, 1.f, 1.f, 91231205);

    for (uint z = 0; z < chunkSizeXZ; ++z)
    {
        for (uint x = 0; x < chunkSizeXZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(x, z);
            const uint height = heightfield[z * chunkSizeXZ + x];

            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(x, z));

            blocks[blockIdx++] = Block::BEDROCK;
            uint y = 1;
            for (; y < height - 5; ++y)
            {
                blocks[blockIdx++] = Block::STONE;
            }
            for (; y < height - 1; ++y)
            {
                blocks[blockIdx++] = Block::DIRT;
            }
            blocks[blockIdx++] = Block::GRASS;

            if (rand1(uvec2(blockPosXZ_WS)) < 0.005f && height < chunkSizeY)
            {
                blocks[blockIdx++] = Block::LAMP;
            }
        }
    }
}

}; // namespace ChunkGenerator
