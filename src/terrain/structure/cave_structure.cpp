// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "cave_structure.h"

#include "../block.h"
#include "../chunk.h"
#include "structure_helpers.h"
#include "util/rng.h"

#include <array>

using namespace glm;
using namespace StructureHelpers;

// All three cave structures are 3x3 in XZ and write a single vertical run per column.
// yStep is +1 to grow upward from a floor anchor, -1 to grow downward from a ceiling anchor.
static void fill3x3VerticalRun(std::vector<Block>& blocks, ivec3 anchorPos_CS, int height, int yStep, Block block)
{
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const ivec2 colPosXZ_CS(anchorPos_CS.x + dx, anchorPos_CS.z + dz);
            if (!Chunk::isInChunkXZ(colPosXZ_CS))
            {
                continue;
            }

            int y = anchorPos_CS.y;
            for (int i = 0; i < height; ++i)
            {
                if (y >= 0 && y < static_cast<int>(chunkSizeY))
                {
                    tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(colPosXZ_CS.x, y, colPosXZ_CS.y /*z*/)), block);
                }
                y += yStep;
            }
        }
    }
}

#define fillCaveStructureBlocksHeader(structureName)                                                                   \
    static void fillCaveStructureBlocks_##structureName(                                                               \
        const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks)

fillCaveStructureBlocksHeader(CRYSTAL)
{
    fill3x3VerticalRun(blocks, structurePos_CS, 5, 1, Block::RAINBOW_CRYSTAL);
}

fillCaveStructureBlocksHeader(HANGING_LAMP)
{
    fill3x3VerticalRun(blocks, structurePos_CS, 5, -1, Block::LAMP);
}

fillCaveStructureBlocksHeader(STONE_COLUMN)
{
    fill3x3VerticalRun(blocks, structurePos_CS, structure.availableHeight, 1, Block::SCALESTONE);
}

CaveStructureBounds::CaveStructureBounds(int diff)
    : minDiffXZ(-diff, -diff), maxDiffXZ(diff, diff)
{}

CaveStructureBounds::CaveStructureBounds(glm::ivec2 minDiffXZ, glm::ivec2 maxDiffXZ)
    : minDiffXZ(minDiffXZ), maxDiffXZ(maxDiffXZ)
{}

namespace CaveStructures
{

using FillCaveStructureFunc = void (*)(const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks);
static std::array<FillCaveStructureFunc, static_cast<size_t>(CaveStructureType::COUNT)> fillCaveStructureFuncs{};

#define FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) fillCaveStructureFuncs[static_cast<size_t>(CaveStructureType::structureName)]
#define SET_FILL_CAVE_STRUCTURE_FUNC(structureName) FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) = fillCaveStructureBlocks_##structureName;

static std::array<CaveStructureBounds, static_cast<size_t>(CaveStructureType::COUNT)> caveStructureBounds{};

#define CAVE_STRUCTURE_BOUNDS_BY_NAME(structureName) caveStructureBounds[static_cast<size_t>(CaveStructureType::structureName)]

void init()
{
    SET_FILL_CAVE_STRUCTURE_FUNC(CRYSTAL);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(CRYSTAL) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(HANGING_LAMP);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(HANGING_LAMP) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(STONE_COLUMN);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(STONE_COLUMN) = 1;

    for (const FillCaveStructureFunc func : fillCaveStructureFuncs)
    {
        ASSERT(func != nullptr);
    }
}

const CaveStructureBounds& getCaveStructureBounds(CaveStructureType type)
{
    return caveStructureBounds[static_cast<size_t>(type)];
}

} // namespace CaveStructures

using namespace CaveStructures;

void Chunk::fillCaveStructureBlocks(const CaveStructure* caveStructures, uint32_t numCaveStructures)
{
    const ivec2 chunkPosBlocksXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    for (uint32_t i = 0; i < numCaveStructures; ++i)
    {
        const CaveStructure& caveStructure = caveStructures[i];

        const ivec2 structurePosXZ_CS = ivec2(caveStructure.pos_WS.x, caveStructure.pos_WS.z) - chunkPosBlocksXZ_WS;
        const CaveStructureBounds& bounds = CaveStructures::getCaveStructureBounds(caveStructure.type);
        const ivec2 structureMinXZ_CS = structurePosXZ_CS + bounds.minDiffXZ;
        const ivec2 structureMaxXZ_CS = structurePosXZ_CS + bounds.maxDiffXZ;

        if (structureMinXZ_CS.x >= static_cast<int>(chunkSizeXZ) || structureMinXZ_CS.y /*z*/ >= static_cast<int>(chunkSizeXZ) ||
            structureMaxXZ_CS.x < 0 || structureMaxXZ_CS.y /*z*/ < 0)
        {
            continue;
        }

        const FillCaveStructureFunc fillCaveStructureFunc = fillCaveStructureFuncs[static_cast<size_t>(caveStructure.type)];
        fillCaveStructureFunc(caveStructure, ivec3(structurePosXZ_CS.x, caveStructure.pos_WS.y, structurePosXZ_CS.y /*z*/), blocks);
    }
}
