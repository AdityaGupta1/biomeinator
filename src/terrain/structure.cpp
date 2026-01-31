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

#include "block.h"
#include "chunk.h"
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

static void fillStructureBlocks_OAK_TREE(const Structure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks)
{
    RandomNumberGenerator rng = initRng(structure.seed);
    ivec3 trunkTopPos_CS = structurePos_CS;
    trunkTopPos_CS.y += rng.nextInt(4, 7);
    if (isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int y = structurePos_CS.y; y <= trunkTopPos_CS.y; ++y)
        {
            blocks[blockIdx++] = Block::OAK_LOG;
        }
    }

    const ivec3 leavesMinPos_CS = glm::max(trunkTopPos_CS - ivec3(2, 2, 2), ivec3(0, 0, 0));
    const ivec3 leavesMaxPos_CS = glm::min(trunkTopPos_CS + ivec3(2, 2, 2), maxPos_CS - 1);
    if (all(lessThanEqual(leavesMinPos_CS, leavesMaxPos_CS)))
    {
        for (int blockZ = leavesMinPos_CS.z; blockZ <= leavesMaxPos_CS.z; ++blockZ)
        {
            for (int blockX = leavesMinPos_CS.x; blockX <= leavesMaxPos_CS.x; ++blockX)
            {
                uint blockIdx = Chunk::blockPosToIdx(uvec3(blockX, leavesMinPos_CS.y, blockZ));

                for (int blockY = leavesMinPos_CS.y; blockY <= leavesMaxPos_CS.y; ++blockY)
                {
                    Block& block = blocks[blockIdx++];
                    if (block == Block::AIR)
                    {
                        block = Block::OAK_LEAVES;
                    }
                }
            }
        }
    }
}

namespace Structures
{

using FillStructureFunc = void (*)(const Structure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks);
static std::array<FillStructureFunc, static_cast<size_t>(StructureType::COUNT)> fillStructureFuncs;

#define FILL_STRUCTURE_FUNC_BY_NAME(structureName) fillStructureFuncs[static_cast<size_t>(StructureType::structureName)]
#define SET_FILL_STRUCTURE_FUNC(structureName) FILL_STRUCTURE_FUNC_BY_NAME(structureName) = fillStructureBlocks_##structureName;

static std::array<StructureBounds, static_cast<size_t>(StructureType::COUNT)> structureBounds;

#define STRUCTURE_BOUNDS_BY_NAME(structureName) structureBounds[static_cast<size_t>(StructureType::structureName)]

void init()
{
    SET_FILL_STRUCTURE_FUNC(OAK_TREE);
    STRUCTURE_BOUNDS_BY_NAME(OAK_TREE) = { ivec3(-2, 0, -2), ivec3(2, 10, 2) };
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

    for (uint32_t i = 0; i < numStructures; i++)
    {
        const Structure& structure = structures[i];
        const FillStructureFunc fillStructureFunc = fillStructureFuncs[static_cast<size_t>(structure.type)];
        const ivec3 structurePos_CS = structure.pos_WS - ivec3(chunkPosBlocksXZ_WS.x, 0, chunkPosBlocksXZ_WS.y /*z*/);
        fillStructureFunc(structure, structurePos_CS, blocks);
    }
}
