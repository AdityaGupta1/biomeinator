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

static FN::SmartNode<FN::Generator> fnTemperature;
static FN::SmartNode<FN::Generator> fnHumidity;
static FN::SmartNode<FN::Generator> fnPeak;
static FN::SmartNode<FN::Generator> fnInland;
inline constexpr float biomeNoiseScale = 1000.f;

static FN::SmartNode<FN::Generator> fnTerrainBase;
static FN::SmartNode<FN::Generator> fnCaves;

static uint worldSeed;
static ivec2 noiseOffsetXZ;

void init()
{
    worldSeed = SettingsManager::getWorldSeed();

    RandomNumberGenerator rng = initRng(worldSeed ^ hash(8810091029));
    noiseOffsetXZ = ivec2(rng.nextInt(-4096, 4096), rng.nextInt(-4096, 4096));

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
        fnFractal->SetOctaveCount(3);

        fnTemperature = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(680199230);
        fnSimplex->SetScale(1.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.04f * biomeNoiseScale);
        fnWarp->SetWarpAmplitude(0.03f * biomeNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(3);

        fnHumidity = fnFractal;
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

        fnPeak = fnFractalRidged;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(76123912);
        fnSimplex->SetScale(5.f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.04f * biomeNoiseScale);
        fnWarp->SetWarpAmplitude(0.02f * biomeNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(5);

        fnInland = fnFractal;
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
    fn->GenUniformGrid2D(data,
                         posXZ.x + noiseOffsetXZ.x,
                         posXZ.y + noiseOffsetXZ.y /*z*/,
                         chunkSizeXZ,
                         chunkSizeXZ,
                         1.f,
                         1.f,
                         worldSeed ^ hash(719023919));
}

static inline void fillNoiseArray3D(float* data, const FN::SmartNode<FN::Generator>& fn, glm::ivec2 posXZ, uint height, int yOffset = 0)
{
    fn->GenUniformGrid3D(data,
                         yOffset /*y*/,
                         posXZ.x + noiseOffsetXZ.x /*x*/,
                         posXZ.y + noiseOffsetXZ.y /*z*/,
                         height,
                         chunkSizeXZ,
                         chunkSizeXZ,
                         1.f,
                         1.f,
                         1.f,
                         worldSeed ^ hash(391023545));
}

}; // namespace ChunkGenerator

using namespace ChunkGenerator;

inline constexpr float terrainBelowHeightfieldSurfaceMultiplier = 2.f;
inline constexpr float surfaceValBound = 1.2f; // noise is approximately between -1 and 1, so +/- 1.2 means we can be absolutely sure that this is terrain or air

inline constexpr int seaLevel = 125;

void Chunk::fillTerrainBlocksAndCreateStructures(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const ivec2 chunkPosBlocksXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    float* temperatureNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* humidityNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* peakNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* inlandNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    fillNoiseArray2D(temperatureNoise, fnTemperature, chunkPosBlocksXZ_WS);
    fillNoiseArray2D(humidityNoise, fnHumidity, chunkPosBlocksXZ_WS);
    fillNoiseArray2D(peakNoise, fnPeak, chunkPosBlocksXZ_WS);
    fillNoiseArray2D(inlandNoise, fnInland, chunkPosBlocksXZ_WS);

    float* terrainBaseHeightArray = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* terrainSurfaceMultiplierArray = threadMemoryAlloc.request<float>(chunkSizeXZSquare);

    int terrainNoiseMinY = chunkSizeY;
    int terrainNoiseMaxY = 0;

    std::set<Biome> biomeSet;

    RandomNumberGenerator rng = initRng(worldSeed ^ hash(330910521), chunkPosBlocksXZ_WS.x, chunkPosBlocksXZ_WS.y /*z*/);

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
                .inland = inlandNoise[columnIdx],
            };
            const Biome biome = Biomes::getClosestBiome(BiomeNoise::randomOffset(biomeNoise, rng));
            this->biomes[columnIdx] = biome;
            biomeSet.insert(biome);

            const float scaledPeak = (biomeNoise.peak + 1.f) * 0.5f;

            float terrainBaseHeight = 140.f;
            {
                terrainBaseHeight += pow(scaledPeak * max(biomeNoise.inland, 0.1f), 4.f) * 135.f;
                const float inlandHeightModifier = 1.f / (1.f + expf(-10.f * biomeNoise.inland + 0.1f)) + 0.03f * biomeNoise.inland - 0.7f;
                terrainBaseHeight += inlandHeightModifier * 90.f;
                const float seaLevelPullFactor = smoothstep(0.2f, 0.0f, abs(biomeNoise.inland)) * 0.9f;
                terrainBaseHeight = glm::mix(terrainBaseHeight, static_cast<float>(seaLevel + 8), seaLevelPullFactor);
            }
            float terrainSurfaceMultiplier = 0.02f;
            {
                terrainSurfaceMultiplier -= scaledPeak * 0.008f;
                terrainSurfaceMultiplier *= 3.f * smoothstep(0.4f, -0.1f, abs(biomeNoise.inland)) + 1.f;
            }

            terrainBaseHeightArray[columnIdx] = terrainBaseHeight;
            terrainSurfaceMultiplierArray[columnIdx] = terrainSurfaceMultiplier;

            const int thisColumnTerrainMinY = static_cast<int>(std::floor(terrainBaseHeight - (surfaceValBound / (terrainSurfaceMultiplier * terrainBelowHeightfieldSurfaceMultiplier))));
            const int thisColumnTerrainMaxY = static_cast<int>(std::ceil(terrainBaseHeight + (surfaceValBound / terrainSurfaceMultiplier)));
            terrainNoiseMinY = std::min(terrainNoiseMinY, thisColumnTerrainMinY);
            terrainNoiseMaxY = std::max(terrainNoiseMaxY, thisColumnTerrainMaxY);
        }
    }

    terrainNoiseMinY = std::max(terrainNoiseMinY, 0);
    terrainNoiseMaxY = std::min(terrainNoiseMaxY, static_cast<int>(chunkSizeY));

    const uint terrainNoiseHeight = terrainNoiseMaxY - terrainNoiseMinY;
    float* terrainNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare * terrainNoiseHeight);
    constexpr uint maxCaveHeight = 160;
    float* caveNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare * maxCaveHeight);
    fillNoiseArray3D(terrainNoise, fnTerrainBase, chunkPosBlocksXZ_WS, terrainNoiseHeight, terrainNoiseMinY);
    fillNoiseArray3D(caveNoise, fnCaves, chunkPosBlocksXZ_WS, maxCaveHeight);

    uint* heightfield = threadMemoryAlloc.request<uint>(chunkSizeXZSquare);

    const uint maxFillY = max(terrainNoiseMaxY, seaLevel);

    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(blockX, blockZ);
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const Biome biome = this->biomes[columnIdx];
            const BiomeData& biomeData = Biomes::getBiomeData(biome);
            const TopBlocks& topBlocks = biomeData.topBlocks;

            const uint baseBlockIdx = chunkSizeY * columnIdx;
            const int baseTerrainNoiseIdx = static_cast<int>(terrainNoiseHeight * columnIdx) - terrainNoiseMinY;
            const uint baseCaveNoiseIdx = maxCaveHeight * columnIdx;

            blocks[baseBlockIdx + 0] = Block::BEDROCK;

            const float terrainBaseHeight = terrainBaseHeightArray[columnIdx];
            const float terrainSurfaceMultiplier = terrainSurfaceMultiplierArray[columnIdx];

            uint topBlockY = 0;
            bool wasSolid = true;
            for (uint y = 1; y <= maxFillY; ++y)
            {
                Block block = Block::AIR;
                const uint blockIdx = baseBlockIdx + y;

                bool isInTerrain;
                if (y < terrainNoiseMinY)
                {
                    isInTerrain = true;
                }
                else
                {
                    float surfaceVal = (terrainBaseHeight - static_cast<float>(y)) * terrainSurfaceMultiplier;
                    if (y < terrainBaseHeight)
                    {
                        surfaceVal *= terrainBelowHeightfieldSurfaceMultiplier; // flatten terrain under base height
                    }

                    isInTerrain = terrainNoise[baseTerrainNoiseIdx + static_cast<int>(y)] < surfaceVal;
                }

                bool isCave = false;
                if (isInTerrain)
                {
                    if (y < maxCaveHeight)
                    {
                        const float caveSurfaceMixFactor =
                            smoothstep<float>(-8, 24, y) * smoothstep<float>(110, 48, y);
                        const float caveSurfaceVal = mix(-0.1f, 0.6f, caveSurfaceMixFactor);
                        isCave = caveNoise[baseCaveNoiseIdx + y] < caveSurfaceVal;
                    }

                    if (!isCave)
                    {
                        const ivec3 blockPos_WS(blockPosXZ_WS.x, y, blockPosXZ_WS.y);
                        RandomNumberGenerator rng =
                            initRng(worldSeed ^ hash(103290193), blockPos_WS.x, blockPos_WS.y, blockPos_WS.z);
                        block = rng.nextFloat() < 0.02f ? Block::LAMP : Block::STONE;
                    }
                }
                else if (y <= seaLevel)
                {
                    block = (y == seaLevel) ? Block::WATER_TOP : Block::WATER;
                }

                this->blocks[blockIdx] = block;

                const bool isSolid = (Blocks::getBlockData(block).type == BlockType::SOLID);
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
                    RandomNumberGenerator rng = initRng(worldSeed ^ hash(87152059),
                                                        gridPosBlocks_WS.x,
                                                        gridPosBlocks_WS.y /*z*/,
                                                        static_cast<uint>(structureGen.type));
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

                    const Biome columnBiome = this->biomes[columnIdx];
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
