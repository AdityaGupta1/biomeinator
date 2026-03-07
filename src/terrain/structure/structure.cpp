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

#include "structure.h"

#include "../block.h"
#include "../chunk.h"
#include "settings_manager.h"
#include "util/rng.h"

#include <array>

using namespace glm;

// TODO: make these into static functions of Chunk?
static inline bool isInChunkXZ(ivec3 pos_CS)
{
    return min(pos_CS.x, pos_CS.z) >= 0 && max(pos_CS.x, pos_CS.z) < chunkSizeXZ;
}

static inline bool isInChunk(ivec3 pos_CS)
{
    return isInChunkXZ(pos_CS) && pos_CS.y >= 0 && pos_CS.y < chunkSizeY;
}

static inline void tryPlaceStructureBlock(std::vector<Block>& blocks, uint blockIdx, Block newBlock)
{
    Block& block = blocks[blockIdx];
    if (block == Block::AIR || block == Block::WATER || block == Block::WATER_TOP)
    {
        block = newBlock;
    }
}

#define fillStructureBlocksHeader(structureName)                                                                       \
    static void fillStructureBlocks_##structureName(                                                                   \
        const Structure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng)

fillStructureBlocksHeader(OAK_TREE)
{
    ivec3 trunkTopPos_CS = structurePos_CS;
    trunkTopPos_CS.y += rng.nextInt(4, 7);
    if (isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int y = structurePos_CS.y; y <= trunkTopPos_CS.y; ++y)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::OAK_LOG);
        }
        for (int dy = 0; dy < 2; ++dy)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::OAK_LEAVES);
        }
    }

    const ivec2 leavesMinPosXZ_CS = glm::max(ivec2(trunkTopPos_CS.x - 2, trunkTopPos_CS.z - 2), ivec2(0, 0));
    const ivec2 leavesMaxPosXZ_CS = glm::min(ivec2(trunkTopPos_CS.x + 2, trunkTopPos_CS.z + 2), ivec2(chunkSizeXZ, chunkSizeXZ) - 1);
    for (int blockZ = leavesMinPosXZ_CS.y /*z*/; blockZ <= leavesMaxPosXZ_CS.y /*z*/; ++blockZ)
    {
        for (int blockX = leavesMinPosXZ_CS.x; blockX <= leavesMaxPosXZ_CS.x; ++blockX)
        {
            uint blockIdx = Chunk::blockPosToIdx(uvec3(blockX, trunkTopPos_CS.y - 1, blockZ));

            ivec2 diffXZ = abs(ivec2(blockX, blockZ) - ivec2(structurePos_CS.x, structurePos_CS.z));
            if (diffXZ.x == 2 && diffXZ.y /*z*/ == 2)
            {
                if (rng.chance(0.5f))
                {
                    if (rng.chance(0.5f))
                    {
                        blockIdx++;
                    }
                    tryPlaceStructureBlock(blocks, blockIdx, Block::OAK_LEAVES);
                }
            }
            else
            {
                int leavesHeight = (diffXZ.x + diffXZ.y == 1) ? 4 : 2;
                for (int dy = 0; dy < leavesHeight; ++dy)
                {
                    tryPlaceStructureBlock(blocks, blockIdx++, Block::OAK_LEAVES);
                }
            }
        }
    }
}

fillStructureBlocksHeader(SAGUARO_CACTUS)
{
    const int trunkHeight = rng.nextInt(4, 10);

    if (isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int dy = 0; dy <= trunkHeight; ++dy)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::CACTUS);
        }
    }

    if (trunkHeight <= 5)
    {
        return;
    }

    constexpr float generateArmChance = 0.4f;
    for (uint dirIdx = 0; dirIdx < 4; ++dirIdx)
    {
        if (!rng.chance(generateArmChance))
        {
            continue;
        }

        const int armBaseHeight = rng.nextInt(2, trunkHeight - 3);
        const int armHeight = rng.nextInt(2, 4);

        const NeighborDirection dir = static_cast<NeighborDirection>(dirIdx);
        const ivec2 dirOffset = neighborOffset(dir);

        const ivec3 armConnectorPos_CS = structurePos_CS + ivec3(dirOffset.x, armBaseHeight, dirOffset.y /*z*/);
        if (isInChunkXZ(armConnectorPos_CS))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(armConnectorPos_CS), Block::CACTUS);
        }

        const ivec3 armBendPos_CS = armConnectorPos_CS + ivec3(dirOffset.x, 0, dirOffset.y /*z*/);
        if (isInChunkXZ(armBendPos_CS))
        {
            uint blockIdx = Chunk::blockPosToIdx(armBendPos_CS);
            for (int dy = 0; dy <= armHeight; ++dy)
            {
                tryPlaceStructureBlock(blocks, blockIdx++, Block::CACTUS);
            }
        }
    }
}

fillStructureBlocksHeader(PALM_TREE)
{
    ivec3 trunkTopPos_CS = structurePos_CS;
    trunkTopPos_CS.y += rng.nextInt(4, 7);
    if (isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int y = structurePos_CS.y; y <= trunkTopPos_CS.y; ++y)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::PALM_LOG);
        }
        for (int dy = 0; dy < 2; ++dy)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::PALM_LEAVES);
        }
    }

    const ivec2 leavesMinPosXZ_CS = glm::max(ivec2(trunkTopPos_CS.x - 2, trunkTopPos_CS.z - 2), ivec2(0, 0));
    const ivec2 leavesMaxPosXZ_CS =
        glm::min(ivec2(trunkTopPos_CS.x + 2, trunkTopPos_CS.z + 2), ivec2(chunkSizeXZ, chunkSizeXZ) - 1);
    for (int blockZ = leavesMinPosXZ_CS.y /*z*/; blockZ <= leavesMaxPosXZ_CS.y /*z*/; ++blockZ)
    {
        for (int blockX = leavesMinPosXZ_CS.x; blockX <= leavesMaxPosXZ_CS.x; ++blockX)
        {
            uint blockIdx = Chunk::blockPosToIdx(uvec3(blockX, trunkTopPos_CS.y - 1, blockZ));

            ivec2 diffXZ = abs(ivec2(blockX, blockZ) - ivec2(structurePos_CS.x, structurePos_CS.z));
            if (diffXZ.x == 2 && diffXZ.y /*z*/ == 2)
            {
                if (rng.chance(0.5f))
                {
                    if (rng.chance(0.5f))
                    {
                        blockIdx++;
                    }
                    tryPlaceStructureBlock(blocks, blockIdx, Block::PALM_LEAVES);
                }
            }
            else
            {
                int leavesHeight = (diffXZ.x + diffXZ.y == 1) ? 4 : 2;
                for (int dy = 0; dy < leavesHeight; ++dy)
                {
                    tryPlaceStructureBlock(blocks, blockIdx++, Block::PALM_LEAVES);
                }
            }
        }
    }
}

StructureBounds::StructureBounds(int diff)
    : minDiffXZ(-diff, -diff), maxDiffXZ(diff, diff)
{}

StructureBounds::StructureBounds(glm::ivec2 minDiffXZ, glm::ivec2 maxDiffXZ)
    : minDiffXZ(minDiffXZ), maxDiffXZ(maxDiffXZ)
{}

namespace Structures
{

using FillStructureFunc = void (*)(const Structure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng);
static std::array<FillStructureFunc, static_cast<size_t>(StructureType::COUNT)> fillStructureFuncs{};

#define FILL_STRUCTURE_FUNC_BY_NAME(structureName) fillStructureFuncs[static_cast<size_t>(StructureType::structureName)]
#define SET_FILL_STRUCTURE_FUNC(structureName) FILL_STRUCTURE_FUNC_BY_NAME(structureName) = fillStructureBlocks_##structureName;

static std::array<StructureBounds, static_cast<size_t>(StructureType::COUNT)> structureBounds{};

#define STRUCTURE_BOUNDS_BY_NAME(structureName) structureBounds[static_cast<size_t>(StructureType::structureName)]

void init()
{
    SET_FILL_STRUCTURE_FUNC(OAK_TREE);
    STRUCTURE_BOUNDS_BY_NAME(OAK_TREE) = 2;

    SET_FILL_STRUCTURE_FUNC(SAGUARO_CACTUS);
    STRUCTURE_BOUNDS_BY_NAME(SAGUARO_CACTUS) = 2;

    SET_FILL_STRUCTURE_FUNC(PALM_TREE);
    STRUCTURE_BOUNDS_BY_NAME(PALM_TREE) = 4;

    for (const FillStructureFunc func : fillStructureFuncs)
    {
        ASSERT(func != nullptr);
    }
}

const StructureBounds& getStructureBounds(StructureType type)
{
    return structureBounds[static_cast<size_t>(type)];
}

} // namespace Structures

using namespace Structures;

void Chunk::fillStructureBlocks(const Structure* structures, uint32_t numStructures)
{
    const ivec2 chunkPosBlocksXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    const uint rngSeed = SettingsManager::getWorldSeed() ^ hash(719266093);

    for (uint32_t i = 0; i < numStructures; ++i)
    {
        const Structure& structure = structures[i];

        const ivec2 structurePosXZ_CS = ivec2(structure.pos_WS.x, structure.pos_WS.z) - chunkPosBlocksXZ_WS;
        const StructureBounds& bounds = Structures::getStructureBounds(structure.type);
        const ivec2 structureMinXZ_CS = structurePosXZ_CS + bounds.minDiffXZ;
        const ivec2 structureMaxXZ_CS = structurePosXZ_CS + bounds.maxDiffXZ;

        if (structureMinXZ_CS.x >= static_cast<int>(chunkSizeXZ) || structureMinXZ_CS.y /*z*/ >= static_cast<int>(chunkSizeXZ) ||
            structureMaxXZ_CS.x < 0 || structureMaxXZ_CS.y /*z*/ < 0)
        {
            continue;
        }

        const FillStructureFunc fillStructureFunc = fillStructureFuncs[static_cast<size_t>(structure.type)];
        RandomNumberGenerator rng = initRng(rngSeed ^ hash(static_cast<uint>(structure.type)), structure.pos_WS.x, structure.pos_WS.y, structure.pos_WS.z);
        fillStructureFunc(structure, ivec3(structurePosXZ_CS.x, structure.pos_WS.y, structurePosXZ_CS.y /*z*/), blocks, rng);
    }
}
