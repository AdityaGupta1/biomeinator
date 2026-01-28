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

static std::vector<FN::SmartNode<FN::Simplex>> climateNoiseFns;

void init()
{
    auto fnTemperature = FN::New<FN::Simplex>();
    fnTemperature->SetSeedOffset(5689481209);
    fnTemperature->SetScale(2500.0f);
    fnTemperature->SetOutputMin(-2.0f);
    fnTemperature->SetOutputMax(2.0f);
    climateNoiseFns.push_back(fnTemperature);

    auto fnRainfall = FN::New<FN::Simplex>();
    fnRainfall->SetSeedOffset(1023950235);
    fnRainfall->SetScale(1000.0f);
    fnRainfall->SetOutputMin(-2.0f);
    fnRainfall->SetOutputMax(2.0f);
    climateNoiseFns.push_back(fnRainfall);

    auto fnHumidity = FN::New<FN::Simplex>();
    fnHumidity->SetSeedOffset(680199230);
    fnHumidity->SetScale(1500.0f);
    fnHumidity->SetOutputMin(-2.0f);
    fnHumidity->SetOutputMax(2.0f);
    climateNoiseFns.push_back(fnHumidity);

    auto fnAltitude = FN::New<FN::Simplex>();
    fnAltitude->SetSeedOffset(973421495);
    fnAltitude->SetScale(2000.0f);
    fnAltitude->SetOutputMin(64.0f);
    fnAltitude->SetOutputMax(256.0f);
    climateNoiseFns.push_back(fnAltitude);
}

void fillBlocks(glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    std::vector<float> climateNoise(climateNoiseFns.size() * chunkSizeXZSquare);
    {
        float* noisePtr = climateNoise.data();
        for (const auto& fn : climateNoiseFns)
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

    std::vector<BiomeWeight> biomeWeights(numClosestBiomes * chunkSizeXZSquare);
    std::unordered_set<Biome> biomeSet;
    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const uint blockIdx = blockZ * chunkSizeXZ + blockX;
            const ClimateVector climateVec = {
                .temperature = climateNoise[0 * chunkSizeXZSquare + blockIdx],
                .rainfall = climateNoise[1 * chunkSizeXZSquare + blockIdx],
                .humidity = climateNoise[2 * chunkSizeXZSquare + blockIdx],
                .altitude = climateNoise[3 * chunkSizeXZSquare + blockIdx],
            };
            BiomeWeight* biomeWeightsPtr = biomeWeights.data() + blockIdx * numClosestBiomes;
            Biomes::getBiomeWeights(climateVec, biomeWeightsPtr);
            for (uint i = 0; i < numClosestBiomes; ++i)
            {
                biomeSet.insert(biomeWeightsPtr[i].biome); // TODO: keep track of extents
            }
        }
    }

    std::unordered_map<Biome, std::vector<float>> biomeHeightfields; // TODO: generate these one at a time and accumulate them into one heightfield
    for (const Biome biome : biomeSet)
    {
        const auto& heightFn = Biomes::getBiomeData(biome).heightFn;
        std::vector<float> heightfield(chunkSizeXZSquare);
        heightFn->GenUniformGrid2D(heightfield.data(),
                                   chunkPosBlocksXZ_WS.x, // x
                                   chunkPosBlocksXZ_WS.y, // z
                                   chunkSizeXZ,
                                   chunkSizeXZ,
                                   1.f,
                                   1.f,
                                   91231205);
        biomeHeightfields[biome] = std::move(heightfield);
    }

    std::vector<float> heightfield(chunkSizeXZSquare);
    std::vector<Biome> columnBiomes(chunkSizeXZSquare);
    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const uint blockIdx = blockZ * chunkSizeXZ + blockX;
            const BiomeWeight* colBiomeWeights = biomeWeights.data() + blockIdx * numClosestBiomes;

            float blendedHeight = 0.0f;
            for (uint i = 0; i < numClosestBiomes; ++i)
            {
                const BiomeWeight& biomeWeight = colBiomeWeights[i];
                blendedHeight += biomeHeightfields[biomeWeight.biome][blockIdx] * biomeWeight.weight;
            }
            heightfield[blockIdx] = blendedHeight;
            columnBiomes[blockIdx] = colBiomeWeights[0].biome;
        }
    }

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
            const uint colIdx = blockZ * chunkSizeXZ + blockX;

            const uint height = heightfield[colIdx];
            const TopBlocks& topBlocks = Biomes::getBiomeData(columnBiomes[colIdx]).topBlocks;

            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(blockX, blockZ));
            uint caveIdx = maxCaveHeight * (blockX + chunkSizeXZ * blockZ) + 1;

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
