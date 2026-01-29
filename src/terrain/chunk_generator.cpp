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
#include "biomes/biome.h"
#include "util/rng.h"

#include <array>
#include <unordered_map>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace ChunkGenerator
{

// TODO: move to biome.cpp
static std::vector<FN::SmartNode<FN::Generator>> biomeNoiseFns;
inline constexpr float biomeNoiseScale = 400.f;

void init()
{
    auto fnTemperature = FN::New<FN::Simplex>();
    fnTemperature->SetSeedOffset(5689481209);
    fnTemperature->SetScale(2.5f * biomeNoiseScale);
    fnTemperature->SetOutputMin(-1.0f);
    fnTemperature->SetOutputMax(1.0f);
    auto fnWarp = FN::New<FN::DomainWarpGradient>();
    fnWarp->SetSource(fnTemperature);
    fnWarp->SetScale(0.06f * biomeNoiseScale);
    fnWarp->SetWarpAmplitude(0.03f * biomeNoiseScale);
    auto fnFractal = FN::New<FN::FractalFBm>();
    fnFractal->SetSource(fnWarp);
    fnFractal->SetOctaveCount(2);
    biomeNoiseFns.push_back(fnFractal);

    auto fnHumidity = FN::New<FN::Simplex>();
    fnHumidity->SetSeedOffset(680199230);
    fnHumidity->SetScale(1.5f * biomeNoiseScale);
    fnHumidity->SetOutputMin(-1.0f);
    fnHumidity->SetOutputMax(1.0f);
    biomeNoiseFns.push_back(fnHumidity);
}

void fillBlocks(glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    // TODO: move to biome.cpp
    auto fnSimplex = FN::New<FN::Simplex>();
    fnSimplex->SetSeedOffset(210393129);
    fnSimplex->SetScale(200.0f);
    auto fnFractal = FN::New<FN::FractalFBm>();
    fnFractal->SetSource(fnSimplex);
    fnFractal->SetOctaveCount(4);
    auto fnMul = FN::New<FN::Multiply>();
    fnMul->SetLHS(fnFractal);
    fnMul->SetRHS(15.0f);
    auto fnAdd = FN::New<FN::Add>();
    fnAdd->SetLHS(fnMul);
    fnAdd->SetRHS(130.0f);

    const auto& fnTerrainBase = fnAdd;

    std::vector<float> biomeNoiseArray(biomeNoiseFns.size() * chunkSizeXZSquare);
    {
        float* noisePtr = biomeNoiseArray.data();
        for (const auto& fn : biomeNoiseFns)
        {
            fn->GenUniformGrid2D(noisePtr,
                                 chunkPosBlocksXZ_WS.x, // x
                                 chunkPosBlocksXZ_WS.y, // z
                                 chunkSizeXZ,
                                 chunkSizeXZ,
                                 1.f,
                                 1.f,
                                 192350424);
            noisePtr += chunkSizeXZSquare;
        }
    }

    std::vector<float> heightfield(chunkSizeXZSquare);
    fnTerrainBase->GenUniformGrid2D(heightfield.data(),
                                    chunkPosBlocksXZ_WS.x, // x
                                    chunkPosBlocksXZ_WS.y, // z
                                    chunkSizeXZ,
                                    chunkSizeXZ,
                                    1.f,
                                    1.f,
                                    91231205);

    static constexpr uint maxCaveHeight = 128;
    std::vector<float> caveNoise(chunkSizeXZSquare * maxCaveHeight);
    {
        auto fnCellular = FN::New<FN::CellularDistance>();
        fnCellular->SetDistanceIndex0(2);
        fnCellular->SetDistanceIndex1(0);
        fnCellular->SetReturnType(FN::CellularDistance::ReturnType::Index0Div1);
        fnCellular->SetScale(80.f);
        auto fnDomainWarp = FN::New<FN::DomainWarpGradient>();
        fnDomainWarp->SetSource(fnCellular);
        fnDomainWarp->SetSeedOffset(302341102);
        fnDomainWarp->SetWarpAmplitude(50.f);
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetScale(1200.f);
        fnSimplex->SetOutputMin(-0.05f);
        fnSimplex->SetOutputMax(0.3f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnDomainWarp);
        fnAdd->SetRHS(fnSimplex);

        fnAdd->GenUniformGrid3D(caveNoise.data(),
                                0, // y
                                chunkPosBlocksXZ_WS.x, // x
                                chunkPosBlocksXZ_WS.y, // z
                                maxCaveHeight,
                                chunkSizeXZ,
                                chunkSizeXZ,
                                1.f,
                                1.f,
                                1.f,
                                559234912);
    }

    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(blockX, blockZ);
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const BiomeNoise biomeNoise = {
                .temperature = biomeNoiseArray[0 * chunkSizeXZSquare + columnIdx],
                .humidity = biomeNoiseArray[1 * chunkSizeXZSquare + columnIdx],
            };
            const Biome biome = Biomes::getBiome(biomeNoise);
            const BiomeData& biomeData = Biomes::getBiomeData(biome);

            const uint height = heightfield[columnIdx];
            const TopBlocks& topBlocks = biomeData.topBlocks;

            uint blockIdx = columnIdx * chunkSizeY;
            uint caveIdx = maxCaveHeight * columnIdx + 1;

            blocks[blockIdx++] = Block::BEDROCK;

            for (uint y = 1; y < height; ++y)
            {
                Block block = Block::AIR;

                bool isCave = false;
                if (y < maxCaveHeight)
                {
                    const float thisCaveNoise = caveNoise[caveIdx++];

                    const float caveIsoSurfaceMixFactor = smoothstep<float>(-8, 24, y) * smoothstep<float>(115, 48, y);
                    const float caveIsoSurface = mix(0.f, 0.7f, caveIsoSurfaceMixFactor);
                    isCave = thisCaveNoise < caveIsoSurface;
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
                        block = topBlocks.mid;
                    }
                    else
                    {
                        block = topBlocks.top;
                    }
                }

                blocks[blockIdx++] = block;
            }

            if (rand1(uvec2(blockPosXZ_WS)) < 0.005f && height < chunkSizeY && blocks[blockIdx - 1] != Block::AIR)
            {
                blocks[blockIdx++] = Block::LAMP;
            }
        }
    }
}

}; // namespace ChunkGenerator
