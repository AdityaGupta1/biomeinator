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
    std::array<float, chunkSizeXZ * chunkSizeXZ> noiseOutput;

    auto fnSimplex = FastNoise::New<FastNoise::Simplex>();
    auto fnFractal = FastNoise::New<FastNoise::FractalFBm>();

    fnFractal->SetSource(fnSimplex);
    fnFractal->SetOctaveCount(4);

    fnFractal->GenUniformGrid2D(
        noiseOutput.data(), chunkPosBlocksXZ_WS.x, chunkPosBlocksXZ_WS.y, chunkSizeXZ, chunkSizeXZ, 1.f, 1.f, 91231205);

    for (uint z = 0; z < chunkSizeXZ; ++z)
    {
        for (uint x = 0; x < chunkSizeXZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(x, z);
            const uint height = 64.f + 10.f * noiseOutput[z * chunkSizeXZ + x];

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
