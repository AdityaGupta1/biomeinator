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

#include "biome.h"
#include "chunk.h"
#include "settings_manager.h"
#include "multithreading/thread_memory_allocator.h"
#include "util/glm_util.h"
#include "util/rng.h"

#include <set>
#include <vector>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace ChunkGenerator
{

static uint32_t worldSeed;

static FN::SmartNode<FN::Generator> fnTemperature;
static FN::SmartNode<FN::Generator> fnHumidity;
static FN::SmartNode<FN::Generator> fnPeak;
inline constexpr float biomeNoiseScale = 1000.f;

static FN::SmartNode<FN::Generator> fnTerrainBase;
static FN::SmartNode<FN::Generator> fnCaves;

void init()
{
    worldSeed = SettingsManager::getAsUint("worldSeed");

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
        auto fnFractalRidged = FN::New<FN::FractalRidged>();
        fnFractalRidged->SetSource(fnSimplex);
        fnFractalRidged->SetOctaveCount(5);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnFractalRidged);
        fnAdd->SetRHS(1.f);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnAdd);
        fnMul->SetRHS(0.5f);

        fnPeak = fnMul;
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
        fnCellular->SetSeedOffset(86839821);
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

static inline void fillNoiseArray2D(float* data, const FN::SmartNode<FN::Generator>& fn, glm::ivec2 posXZ)
{
    fn->GenUniformGrid2D(data, posXZ.x, posXZ.y /*z*/, chunkSizeXZ, chunkSizeXZ, 1.f, 1.f, worldSeed);
}

static inline void fillNoiseArray3D(float* data, const FN::SmartNode<FN::Generator>& fn, glm::ivec2 posXZ, uint height)
{
    fn->GenUniformGrid3D(data, 0 /*y*/, posXZ.x /*x*/, posXZ.y /*z*/, height, chunkSizeXZ, chunkSizeXZ, 1.f, 1.f, 1.f, worldSeed);
}

}; // namespace ChunkGenerator

using namespace ChunkGenerator;

void Chunk::fillTerrainBlocksAndCreateStructures(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const ivec2 chunkPosBlocksXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    float* temperatureNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* humidityNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* peakNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    fillNoiseArray2D(temperatureNoise, fnTemperature, chunkPosBlocksXZ_WS);
    fillNoiseArray2D(humidityNoise, fnHumidity, chunkPosBlocksXZ_WS);
    fillNoiseArray2D(peakNoise, fnPeak, chunkPosBlocksXZ_WS);

    constexpr uint terrainNoiseMinY = 80;
    constexpr uint terrainNoiseHeight = chunkSizeY - terrainNoiseMinY;
    float* terrainNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare * terrainNoiseHeight);
    constexpr uint maxCaveHeight = 160;
    float* caveNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare * maxCaveHeight);
    fillNoiseArray3D(terrainNoise, fnTerrainBase, chunkPosBlocksXZ_WS, terrainNoiseHeight);
    fillNoiseArray3D(caveNoise, fnCaves, chunkPosBlocksXZ_WS, maxCaveHeight);

    uint* heightfield = threadMemoryAlloc.request<uint>(chunkSizeXZSquare);
    Biome* biomes = threadMemoryAlloc.request<Biome>(chunkSizeXZSquare);
    std::set<Biome> biomeSet;

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
            biomes[columnIdx] = biome;
            biomeSet.insert(biome);
            const BiomeData& biomeData = Biomes::getBiomeData(biome);

            const TopBlocks& topBlocks = biomeData.topBlocks;

            const uint baseBlockIdx = chunkSizeY * columnIdx;
            const int baseTerrainNoiseIdx = terrainNoiseHeight * columnIdx - terrainNoiseMinY;
            const uint baseCaveNoiseIdx = maxCaveHeight * columnIdx;

            blocks[baseBlockIdx + 0] = Block::BEDROCK;

            const float terrainBaseHeight = 100.f + powf(biomeNoise.peak, 3.f) * 165.f;
            const float terrainSurfaceMultiplier = 0.02f - biomeNoise.peak * 0.008f;

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

                bool isInTerrain = y < terrainNoiseMinY || (terrainNoise[baseTerrainNoiseIdx + y] < surfaceVal);
                bool isCave = false;
                if (isInTerrain)
                {
                    if (y < maxCaveHeight)
                    {
                        const float caveSurfaceMixFactor =
                            smoothstep<float>(-8, 24, y) * smoothstep<float>(115, 48, y);
                        const float caveSurfaceVal = mix(0.f, 0.6f, caveSurfaceMixFactor);
                        isCave = caveNoise[baseCaveNoiseIdx + y] < caveSurfaceVal;
                    }

                    if (!isCave)
                    {
                        const ivec3 blockPos_WS(blockPosXZ_WS.x, y, blockPosXZ_WS.y);
                        RandomNumberGenerator rng = initRng(blockPos_WS.x, blockPos_WS.y, blockPos_WS.z);
                        block = rng.nextFloat() < 0.02f ? Block::LAMP : Block::STONE;
                    }
                }

                this->blocks[blockIdx] = block;

                const bool isSolid = (block != Block::AIR);
                if (wasSolid && !isSolid && !isCave)
                {
                    topBlockY = y - 1;
                }
                wasSolid = isSolid;
            }

            if (topBlockY != 0)
            {
                for (uint y = topBlockY; y > topBlockY - 5; --y)
                {
                    const uint blockIdx = baseBlockIdx + y;
                    Block& block = this->blocks[blockIdx];
                    if (block == Block::AIR || block == Block::BEDROCK)
                    {
                        break;
                    }

                    const Block newBlock = (y == topBlockY) ? topBlocks.top : topBlocks.mid;
                    block = newBlock;
                }
            }

            heightfield[columnIdx] = topBlockY;
        }
    }

    const ivec2 chunkEndPosBlocksXZ_WS = chunkPosBlocksXZ_WS + static_cast<int>(chunkSizeXZ);

    for (Biome biome : biomeSet)
    {
        const BiomeData& biomeData = Biomes::getBiomeData(biome);
        for (const StructureGen& structureGen : biomeData.structureGens)
        {
            const uint gridCellSideLength = structureGen.gridCellSideLength;

            const ivec2 minGridPos = glmUtil::floorDiv(chunkPosBlocksXZ_WS, ivec2(gridCellSideLength)); // inclusive
            const ivec2 maxGridPos = glmUtil::floorDiv(chunkEndPosBlocksXZ_WS - 1, ivec2(gridCellSideLength)); // inclusive

            ASSERT(structureGen.minRadius < gridCellSideLength);

            const ivec2 paddedMinGridPos = minGridPos - 1;
            const ivec2 paddedMaxGridPos = maxGridPos + 1;

            const uint paddedNumGridCellsX = paddedMaxGridPos.x - paddedMinGridPos.x + 1;
            const uint paddedNumGridCellsZ = paddedMaxGridPos.y /*z*/ - paddedMinGridPos.y /*z*/ + 1;
            ivec2* candidatePositionsXZ_WS = threadMemoryAlloc.request<ivec2>(paddedNumGridCellsX * paddedNumGridCellsZ);

            uint candidatePosIdx = 0;
            for (int gridZ = paddedMinGridPos.y /*z*/; gridZ <= paddedMaxGridPos.y /*z*/; ++gridZ)
            {
                for (int gridX = paddedMinGridPos.x; gridX <= paddedMaxGridPos.x; ++gridX)
                {
                    const ivec2 gridPosBlocks_WS = ivec2(gridX, gridZ) * static_cast<int>(gridCellSideLength);
                    RandomNumberGenerator rng =
                        initRng(gridPosBlocks_WS.x, gridPosBlocks_WS.y /*z*/, static_cast<uint>(structureGen.type), 87152059);
                    const ivec2 candidatePosXZ_WS =
                        gridPosBlocks_WS + ivec2(rng.nextInt(gridCellSideLength), rng.nextInt(gridCellSideLength));
                    candidatePositionsXZ_WS[candidatePosIdx++] = candidatePosXZ_WS;
                }
            }

            const float r = structureGen.minRadius;
            const float r2 = r * r;

            for (int gridZ = minGridPos.y; gridZ <= maxGridPos.y; ++gridZ)
            {
                const int zOffset = gridZ - paddedMinGridPos.y;

                for (int gridX = minGridPos.x; gridX <= maxGridPos.x; ++gridX)
                {
                    const int xOffset = gridX - paddedMinGridPos.x;
                    const uint candidatePosIdx = xOffset + paddedNumGridCellsX * zOffset;

                    const ivec2 candidatePosXZ_WS = candidatePositionsXZ_WS[candidatePosIdx];
                    const ivec2 candidatePosXZ_CS = candidatePosXZ_WS - chunkPosBlocksXZ_WS;
                    if (!Chunk::isPosInBounds(candidatePosXZ_CS))
                    {
                        continue;
                    }

                    const uint columnIdx = candidatePosXZ_CS.x + chunkSizeXZ * candidatePosXZ_CS.y /*z*/;

                    const uint candidateGroundHeight = heightfield[columnIdx];
                    if (candidateGroundHeight == 0)
                    {
                        continue; // top of this column is a cave, so skip this candidate
                    }

                    const Biome columnBiome = biomes[columnIdx];
                    if (columnBiome != biome)
                    {
                        continue;
                    }

                    // check neighbors
                    // note that this does not check distance between candidates of different types; I will revisit this if it becomes a noticeable issue
                    {
                        bool tooClose = false;

                        for (int nGridZ = gridZ - 1; nGridZ <= gridZ + 1; ++nGridZ)
                        {
                            const uint nZOffset = nGridZ - paddedMinGridPos.y /*z*/;
                            if (nZOffset < 0 || nZOffset >= paddedNumGridCellsZ)
                            {
                                continue;
                            }

                            for (int nGridX = gridX - 1; nGridX <= gridX + 1; ++nGridX)
                            {
                                const uint nXOffset = nGridX - paddedMinGridPos.x;
                                if (nXOffset < 0 || nXOffset >= paddedNumGridCellsX)
                                {
                                    continue;
                                }

                                if (nGridX == gridX && nGridZ == gridZ)
                                {
                                    continue;
                                }

                                const uint nIdx = nZOffset * paddedNumGridCellsX + nXOffset;
                                const ivec2 nPosXZ_WS = candidatePositionsXZ_WS[nIdx];

                                const ivec2 d = nPosXZ_WS - candidatePosXZ_WS;
                                const float dist2 = d.x * d.x + d.y * d.y;
                                if (dist2 < r2)
                                {
                                    tooClose = true;
                                    break;
                                }
                            }

                            if (tooClose)
                            {
                                break;
                            }
                        }

                        if (tooClose)
                        {
                            continue;
                        }
                    }

                    const ivec3 candidatePos_WS = ivec3(candidatePosXZ_WS.x, candidateGroundHeight + 1, candidatePosXZ_WS.y /*z*/);
                    this->structures.emplace_back(structureGen.type, candidatePos_WS);
                }
            }
        }
    }
}
