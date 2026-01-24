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
namespace FN = FastNoise;

namespace ChunkGenerator
{

void fillBlocks(glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    std::array<float, chunkSizeXZ * chunkSizeXZ> heightfield;
    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetScale(200.f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(24.f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(80.f);

        fnAdd->GenUniformGrid2D(heightfield.data(),
                                chunkPosBlocksXZ_WS.x, // x
                                chunkPosBlocksXZ_WS.y, // z
                                chunkSizeXZ,
                                chunkSizeXZ,
                                1.f,
                                1.f,
                                91231205);
    }

    static constexpr uint maxCaveHeight = 96;
    std::array<float, chunkSizeXZ * maxCaveHeight * chunkSizeXZ> caveNoise;
    {
        auto fnCellular = FN::New<FN::CellularDistance>();
        fnCellular->SetDistanceIndex0(2);
        fnCellular->SetDistanceIndex1(0);
        fnCellular->SetReturnType(FN::CellularDistance::ReturnType::Index0Div1);

        fnCellular->GenUniformGrid3D(caveNoise.data(),
                                     0,                     // y
                                     chunkPosBlocksXZ_WS.x, // x
                                     chunkPosBlocksXZ_WS.y, // z
                                     maxCaveHeight,
                                     chunkSizeXZ,
                                     chunkSizeXZ,
                                     1.f,
                                     1.f,
                                     1.f,
                                     859234912);
    }

    for (uint z = 0; z < chunkSizeXZ; ++z)
    {
        for (uint x = 0; x < chunkSizeXZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(x, z);
            const uint height = heightfield[z * chunkSizeXZ + x];

            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(x, z));

            blocks[blockIdx++] = Block::BEDROCK;

            for (uint y = 1; y < height; ++y)
            {
                Block block = Block::AIR;

                bool isCave = false;
                if (y < maxCaveHeight)
                {
                    const uint caveIdx = y + (maxCaveHeight * (x + chunkSizeXZ * z)); // TODO: calculate once outside and increment within loop
                    const float thisCaveNoise = caveNoise[caveIdx];
                    if (thisCaveNoise < 0.5f)
                    {
                        isCave = true;
                    }
                }

                if (!isCave)
                {
                    const ivec3 blockPos_WS(blockPosXZ_WS.x, y, blockPosXZ_WS.y);

                    if (y < height - 5)
                    {
                        block = rand1(uvec3(blockPos_WS)) < 0.02f ? Block::LAMP : Block::STONE;
                    }
                    else if (y < height - 1)
                    {
                        block = Block::DIRT;
                    }
                    else
                    {
                        block = Block::GRASS;
                    }
                }

                blocks[blockIdx++] = block;
            }

            if (rand1(uvec2(blockPosXZ_WS)) < 0.005f && height < chunkSizeY)
            {
                blocks[blockIdx++] = Block::LAMP;
            }
        }
    }
}

}; // namespace ChunkGenerator
