// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "cave_structure.h"

#include "../block.h"
#include "../chunk.h"
#include "settings_manager.h"
#include "structure_helpers.h"
#include "util/rng.h"

#include <algorithm>
#include <array>

using namespace glm;
using namespace StructureHelpers;

// The 3x3 cave structures write a single vertical run per column.
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

inline constexpr int caveVinesMinRadius = 3;
inline constexpr int caveVinesMaxRadius = 6;
inline constexpr int caveVinesMinStrandHeight = 3;
inline constexpr int caveVinesMaxStrandHeight = 12;
inline constexpr float caveVinesCenterStrandChance = 0.6f;
inline constexpr float caveVinesEdgeStrandChance = 0.35f;
inline constexpr float caveVinesBerryChance = 0.25f;
// How far a column's own ceiling may differ from the anchor's before the column is skipped,
// which keeps a cluster on one ceiling surface instead of reaching into pockets above or below
inline constexpr int caveVinesCeilingSearchDist = 6;

// Only a solid cube can anchor a strand; an X-shaped block (e.g. another cluster's vine) can't.
static bool isCaveVinesCeilingBlock(Block block)
{
    const BlockData& blockData = Blocks::getBlockData(block);
    return blockData.type == BlockType::SOLID && blockData.shape == BlockShape::CUBE;
}

// Finds the topmost air block under this column's ceiling by searching from the anchor y.
static bool findCaveVinesStrandStartY(const std::vector<Block>& blocks, ivec2 colPosXZ_CS, int anchorY, int& outStartY)
{
    const auto blockAt = [&](int y)
    {
        return blocks[Chunk::blockPosToIdx(uvec3(colPosXZ_CS.x, y, colPosXZ_CS.y /*z*/))];
    };
    const int maxY = static_cast<int>(chunkSizeY) - 1;

    if (blockAt(anchorY) == Block::AIR)
    {
        for (int y = anchorY; y < anchorY + caveVinesCeilingSearchDist && y < maxY; ++y)
        {
            const Block above = blockAt(y + 1);
            if (above == Block::AIR)
            {
                continue;
            }
            if (!isCaveVinesCeilingBlock(above))
            {
                return false;
            }
            outStartY = y;
            return true;
        }
        return false;
    }

    for (int y = anchorY - 1; y >= anchorY - caveVinesCeilingSearchDist && y >= 0; --y)
    {
        if (blockAt(y) == Block::AIR)
        {
            if (!isCaveVinesCeilingBlock(blockAt(y + 1)))
            {
                return false;
            }
            outStartY = y;
            return true;
        }
    }
    return false;
}

// Hangs strands from the ceiling within a circle; strand chance falls off toward the circle edge.
fillCaveStructureBlocksHeader(CAVE_VINES)
{
    const uint worldSeed = SettingsManager::getWorldSeed();
    // Leave at least one air block below the longest strand
    const int maxStrandHeight = std::min(caveVinesMaxStrandHeight, structure.availableHeight - 1);

    // Seeded from the anchor position so every chunk this structure overlaps rolls the same radius
    RandomNumberGenerator structureRng = initRng(worldSeed ^ hash(2093481127),
                                                 static_cast<uint32_t>(structure.pos_WS.x),
                                                 static_cast<uint32_t>(structure.pos_WS.y),
                                                 static_cast<uint32_t>(structure.pos_WS.z));
    const int radius = structureRng.nextInt(caveVinesMinRadius, caveVinesMaxRadius + 1);

    for (int dz = -radius; dz <= radius; ++dz)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const float distFromCenter = glm::length(vec2(dx, dz));
            if (distFromCenter > static_cast<float>(radius))
            {
                continue;
            }

            const ivec2 colPosXZ_CS(structurePos_CS.x + dx, structurePos_CS.z + dz);
            if (!Chunk::isInChunkXZ(colPosXZ_CS))
            {
                continue;
            }

            // Seeded from world position so every chunk this structure overlaps produces the same strand
            RandomNumberGenerator rng = initRng(worldSeed ^ hash(1497203641),
                                                static_cast<uint32_t>(structure.pos_WS.x + dx),
                                                static_cast<uint32_t>(structure.pos_WS.z + dz),
                                                static_cast<uint32_t>(structure.pos_WS.y));
            const float strandChance =
                glm::mix(caveVinesCenterStrandChance, caveVinesEdgeStrandChance, distFromCenter / radius);
            if (!rng.chance(strandChance))
            {
                continue;
            }

            const int strandHeight = rng.nextInt(caveVinesMinStrandHeight, maxStrandHeight + 1);
            int strandStartY;
            if (!findCaveVinesStrandStartY(blocks, colPosXZ_CS, structurePos_CS.y, strandStartY))
            {
                continue;
            }

            uint32_t lastVineIdx = 0;
            int numPlaced = 0;
            for (int i = 0; i < strandHeight; ++i)
            {
                const int y = strandStartY - i;
                // Keep one air block between the strand and whatever is below it, matching the
                // anchor column's availableHeight - 1 cap
                if (y < 1)
                {
                    break;
                }
                const uint32_t blockIdx = Chunk::blockPosToIdx(uvec3(colPosXZ_CS.x, y, colPosXZ_CS.y /*z*/));
                const uint32_t belowBlockIdx = Chunk::blockPosToIdx(uvec3(colPosXZ_CS.x, y - 1, colPosXZ_CS.y /*z*/));
                if (blocks[blockIdx] != Block::AIR || blocks[belowBlockIdx] != Block::AIR)
                {
                    break;
                }
                blocks[blockIdx] = rng.chance(caveVinesBerryChance) ? Block::CAVE_VINES_BERRIES : Block::CAVE_VINES;
                lastVineIdx = blockIdx;
                ++numPlaced;
            }
            if (numPlaced > 0)
            {
                blocks[lastVineIdx] = (blocks[lastVineIdx] == Block::CAVE_VINES_BERRIES)
                    ? Block::CAVE_VINES_TIP_BERRIES
                    : Block::CAVE_VINES_TIP;
            }
        }
    }
}

namespace CaveStructures
{

using FillCaveStructureFunc = void (*)(const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks);
static std::array<FillCaveStructureFunc, static_cast<size_t>(CaveStructureType::COUNT)> fillCaveStructureFuncs{};

#define FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) fillCaveStructureFuncs[static_cast<size_t>(CaveStructureType::structureName)]
#define SET_FILL_CAVE_STRUCTURE_FUNC(structureName) FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) = fillCaveStructureBlocks_##structureName;

static std::array<StructureBounds, static_cast<size_t>(CaveStructureType::COUNT)> caveStructureBounds{};

#define CAVE_STRUCTURE_BOUNDS_BY_NAME(structureName) caveStructureBounds[static_cast<size_t>(CaveStructureType::structureName)]

void init()
{
    SET_FILL_CAVE_STRUCTURE_FUNC(CRYSTAL);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(CRYSTAL) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(HANGING_LAMP);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(HANGING_LAMP) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(STONE_COLUMN);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(STONE_COLUMN) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(CAVE_VINES);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(CAVE_VINES) = caveVinesMaxRadius;

    for (const FillCaveStructureFunc func : fillCaveStructureFuncs)
    {
        ASSERT(func != nullptr);
    }
}

const StructureBounds& getCaveStructureBounds(CaveStructureType type)
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
        const StructureBounds& bounds = CaveStructures::getCaveStructureBounds(caveStructure.type);
        const ivec2 structureMinXZ_CS = structurePosXZ_CS + bounds.minDiffXZ;
        const ivec2 structureMaxXZ_CS = structurePosXZ_CS + bounds.maxDiffXZ;

        if (structureAabbRejectsChunk(structureMinXZ_CS, structureMaxXZ_CS))
        {
            continue;
        }

        const FillCaveStructureFunc fillCaveStructureFunc = fillCaveStructureFuncs[static_cast<size_t>(caveStructure.type)];
        fillCaveStructureFunc(caveStructure, ivec3(structurePosXZ_CS.x, caveStructure.pos_WS.y, structurePosXZ_CS.y /*z*/), blocks);
    }
}
