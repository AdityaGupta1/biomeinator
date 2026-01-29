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

#include <vector>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace ChunkGenerator
{

static FN::SmartNode<FN::Generator> fnTemperature;
static FN::SmartNode<FN::Generator> fnHumidity;
static FN::SmartNode<FN::Generator> fnPeak;
inline constexpr float biomeNoiseScale = 1000.f;

static FN::SmartNode<FN::Generator> fnTerrainBase;
static FN::SmartNode<FN::Generator> fnCaves;

void init()
{
    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(5689481209);
        fnSimplex->SetScale(2.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.06f * biomeNoiseScale);
        fnWarp->SetWarpAmplitude(0.02f * biomeNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(2);

        fnTemperature = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(680199230);
        fnSimplex->SetScale(1.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);

        fnHumidity = fnSimplex;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(901992021);
        fnSimplex->SetScale(2.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(0.0f);
        fnSimplex->SetOutputMax(1.0f);

        fnPeak = fnSimplex;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(210393129);
        fnSimplex->SetScale(400.0f);
        fnSimplex->SetOutputMin(-0.5f);
        fnSimplex->SetOutputMax(0.5f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(5);

        fnTerrainBase = fnFractal;
    }

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
        fnSimplex->SetScale(800.f);
        fnSimplex->SetOutputMin(-0.3f);
        fnSimplex->SetOutputMax(0.3f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnDomainWarp);
        fnAdd->SetRHS(fnSimplex);

        fnCaves = fnAdd;
    }
}

static inline void fillNoiseArray(std::vector<float>& data, const FN::SmartNode<FN::Generator>& fn, glm::ivec2 pos)
{
    ASSERT(data.size() >= chunkSizeXZSquare);
    fn->GenUniformGrid2D(data.data(), pos.x, pos.y /*z*/, chunkSizeXZ, chunkSizeXZ, 1.f, 1.f, 192350424);
}

// TODO: make a generic spline interpolation function/class
static inline float peakNoiseSpline(float t)
{
    if (t < 0.8f)
    {
        return t * 0.5f;
    }
    else
    {
        return 0.4f + (t - 0.8f) * 3.f;
    }
}

void fillBlocks(glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    std::vector<float> temperatureNoise(chunkSizeXZSquare); // TODO: pass in preallocated memory for noise
    fillNoiseArray(temperatureNoise, fnTemperature, chunkPosBlocksXZ_WS);

    std::vector<float> humidityNoise(chunkSizeXZSquare);
    fillNoiseArray(humidityNoise, fnHumidity, chunkPosBlocksXZ_WS);

    std::vector<float> peakNoise(chunkSizeXZSquare);
    fillNoiseArray(peakNoise, fnPeak, chunkPosBlocksXZ_WS);

    std::vector<float> terrainNoise(numChunkBlocks);
    fnTerrainBase->GenUniformGrid3D(terrainNoise.data(),
                                    0, // y
                                    chunkPosBlocksXZ_WS.x, // x
                                    chunkPosBlocksXZ_WS.y, // z
                                    chunkSizeY,
                                    chunkSizeXZ,
                                    chunkSizeXZ,
                                    1.f,
                                    1.f,
                                    1.f,
                                    91231205);

    static constexpr uint maxCaveHeight = 160;
    std::vector<float> caveNoise(chunkSizeXZSquare * maxCaveHeight);
    fnCaves->GenUniformGrid3D(caveNoise.data(),
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

    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(blockX, blockZ);
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const BiomeNoise biomeNoise = {
                .temperature = temperatureNoise[columnIdx],
                .humidity = humidityNoise[columnIdx],
                .peak = peakNoise[columnIdx],
            };
            const Biome biome = Biomes::getClosestBiome(biomeNoise);
            const BiomeData& biomeData = Biomes::getBiomeData(biome);

            const TopBlocks& topBlocks = biomeData.topBlocks;

            uint baseBlockIdx = chunkSizeY * columnIdx;
            uint baseCaveIdx = maxCaveHeight * columnIdx;

            blocks[baseBlockIdx + 0] = Block::BEDROCK;

            const float terrainBaseHeight = 100.f + powf(biomeNoise.peak, 3.f) * 150.f;
            const float terrainSurfaceMultiplier = 0.02f - biomeNoise.peak * 0.012f;

            uint topBlockY = 0;
            bool wasSolid = true;
            for (uint y = 1; y < chunkSizeY; ++y)
            {
                Block block = Block::AIR;
                const uint blockIdx = baseBlockIdx + y;

                float surfaceVal = (terrainBaseHeight - static_cast<float>(y)) * terrainSurfaceMultiplier;
                if (y < terrainBaseHeight)
                {
                    surfaceVal *= 2.0f;
                }

                if (surfaceVal < -1.2f)
                {
                    break;
                }

                bool isInTerrain = (terrainNoise[blockIdx] < surfaceVal);
                bool isCave = false;
                if (isInTerrain)
                {
                    if (y < maxCaveHeight)
                    {
                        const float caveSurfaceMixFactor =
                            smoothstep<float>(-8, 24, y) * smoothstep<float>(115, 48, y);
                        const float caveSurfaceVal = mix(0.f, 0.6f, caveSurfaceMixFactor);
                        isCave = caveNoise[baseCaveIdx + y] < caveSurfaceVal;
                    }

                    if (!isCave)
                    {
                        const ivec3 blockPos_WS(blockPosXZ_WS.x, y, blockPosXZ_WS.y);

                        block = rand1(uvec3(blockPos_WS)) < 0.02f ? Block::LAMP : Block::STONE;
                    }
                }

                blocks[blockIdx] = block;

                const bool isSolid = (block != Block::AIR);
                if (wasSolid && !isSolid && !isCave)
                {
                    topBlockY = y - 1;
                }
                wasSolid = isSolid;
            }

            for (uint y = topBlockY; y > topBlockY - 5; --y)
            {
                const uint blockIdx = baseBlockIdx + y;
                const Block prevBlock = blocks[blockIdx];
                if (prevBlock == Block::AIR || prevBlock == Block::BEDROCK)
                {
                    break;
                }

                const Block newBlock = (y == topBlockY) ? topBlocks.top : topBlocks.mid;
                blocks[blockIdx] = newBlock;
            }

            const uint candidateLampPosY = topBlockY + 1;
            const ivec3 candidateLampPos = ivec3(blockPosXZ_WS.x, candidateLampPosY, blockPosXZ_WS.y /*z*/);
            if (candidateLampPosY < chunkSizeY && rand1(uvec3(candidateLampPos)) < 0.005f)
            {
                blocks[baseBlockIdx + candidateLampPosY] = Block::LAMP;
            }
        }
    }
}

}; // namespace ChunkGenerator
