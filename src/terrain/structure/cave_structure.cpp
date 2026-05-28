// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "cave_structure.h"

#include "../block.h"
#include "../chunk.h"
#include "settings_manager.h"
#include "structure_helpers.h"
#include "util/rng.h"

#include <array>

using namespace glm;
using namespace StructureHelpers;

// Matches tryPlaceStructureBlock's overwrite set; used by OBSIDIAN_STALACTITE's ceiling clip
// to decide whether the block above a candidate voxel counts as solid overhang.
static inline bool isAirLikeBlock(Block b)
{
    return b == Block::AIR || b == Block::WATER || b == Block::WATER_TOP;
}

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
        const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng)

fillCaveStructureBlocksHeader(CRYSTAL)
{
    fill3x3VerticalRun(blocks, structurePos_CS, 5, 1, Block::RAINBOW_CRYSTAL);
}

// Hangs from a ceiling anchor; base is widest at the top (anchor.y) and tapers linearly to a
// single-block tip whose voxel is a LAMP. Base diameter and total height are randomized per
// structure from the per-structure rng. The center also follows a clamped 2D random walk for
// the top half of the height, then freezes — so the stalactite leans at the top and hangs
// straight from the midpoint down. The clamp keeps total drift within the StructureBounds,
// so the AABB rejection stays correct.
//
// Ceiling-clip with 1-block bridge: a voxel is only written if the block directly above (y+1)
// is solid. If y+1 is air but y+2 is solid, the 1-block dip is bridged with an extra OBSIDIAN
// at y+1 and the voxel is written normally. If both are air, the voxel is skipped — since
// fill runs top-down, that clears the column underneath via cascade, so the stalactite trims
// to whatever local ceiling exists and never floats with an exposed top.
//
// Bottom half rolls a per-voxel LAMP chance that ramps linearly from 0% at the midpoint to
// 100% at the tip, so the stem glows brighter toward the bottom. The final tip voxel is
// always LAMP — and because the clip applies to it, a clipped column produces no floating
// tip-lamp.
fillCaveStructureBlocksHeader(OBSIDIAN_STALACTITE)
{
    const int height = rng.nextInt(8, 13);
    const float baseDiameter = rng.nextFloat(4.f, 6.f);
    const int tipOffset = height - 1;
    const int walkEnd = height / 2;
    const int bottomHalfStart = height / 2;

    constexpr float walkStepMax = 0.5f;
    constexpr float walkClamp = 2.f;
    glm::vec2 offset(0.f);

    for (int i = 0; i < height; ++i)
    {
        if (i > 0 && i < walkEnd)
        {
            offset.x = glm::clamp(offset.x + rng.nextFloatAbs(walkStepMax), -walkClamp, walkClamp);
            offset.y = glm::clamp(offset.y + rng.nextFloatAbs(walkStepMax), -walkClamp, walkClamp);
        }

        const int y = structurePos_CS.y - i;
        const int centerX = structurePos_CS.x + static_cast<int>(glm::round(offset.x));
        const int centerZ = structurePos_CS.z + static_cast<int>(glm::round(offset.y));

        const float t = static_cast<float>(i) / static_cast<float>(tipOffset);
        const float diameter = glm::mix(baseDiameter, 1.f, t);
        const float radius = diameter * 0.5f;
        const float radius2 = radius * radius;
        const int radiusCeil = static_cast<int>(glm::ceil(radius));

        const bool inBottomHalf = (i >= bottomHalfStart);
        const bool isTipLayer = (i == tipOffset);

        for (int dz = -radiusCeil; dz <= radiusCeil; ++dz)
        {
            for (int dx = -radiusCeil; dx <= radiusCeil; ++dx)
            {
                if (dx * dx + dz * dz >= radius2)
                {
                    continue;
                }
                const ivec3 pos_CS(centerX + dx, y, centerZ + dz);
                if (!Chunk::isInChunk(pos_CS))
                {
                    continue;
                }

                // Clip with 1-block bridge. y+1 air + y+2 solid → place OBSIDIAN at y+1 to
                // bridge the gap, then write the voxel at y as normal.
                const uint32_t oneAboveIdx =
                    Chunk::blockPosToIdx(uvec3(pos_CS.x, pos_CS.y + 1, pos_CS.z));
                if (isAirLikeBlock(blocks[oneAboveIdx]))
                {
                    const int yTwoAbove = pos_CS.y + 2;
                    if (yTwoAbove >= static_cast<int>(chunkSizeY))
                    {
                        continue;
                    }
                    const uint32_t twoAboveIdx = Chunk::blockPosToIdx(uvec3(pos_CS.x, yTwoAbove, pos_CS.z));
                    if (isAirLikeBlock(blocks[twoAboveIdx]))
                    {
                        continue;
                    }
                    tryPlaceStructureBlock(blocks, oneAboveIdx, Block::OBSIDIAN);
                }

                Block voxelBlock;
                if (isTipLayer)
                {
                    voxelBlock = Block::LAMP;
                }
                else if (inBottomHalf)
                {
                    const float lampChance =
                        static_cast<float>(i - bottomHalfStart) / static_cast<float>(tipOffset - bottomHalfStart);
                    voxelBlock = rng.chance(lampChance) ? Block::LAMP : Block::OBSIDIAN;
                }
                else
                {
                    voxelBlock = Block::OBSIDIAN;
                }

                tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(pos_CS)), voxelBlock);
            }
        }
    }
}

fillCaveStructureBlocksHeader(STONE_COLUMN)
{
    fill3x3VerticalRun(blocks, structurePos_CS, structure.availableHeight, 1, Block::SCALESTONE);
}

namespace CaveStructures
{

using FillCaveStructureFunc = void (*)(
    const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng);
static std::array<FillCaveStructureFunc, static_cast<size_t>(CaveStructureType::COUNT)> fillCaveStructureFuncs{};

#define FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) fillCaveStructureFuncs[static_cast<size_t>(CaveStructureType::structureName)]
#define SET_FILL_CAVE_STRUCTURE_FUNC(structureName) FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) = fillCaveStructureBlocks_##structureName;

static std::array<StructureBounds, static_cast<size_t>(CaveStructureType::COUNT)> caveStructureBounds{};

#define CAVE_STRUCTURE_BOUNDS_BY_NAME(structureName) caveStructureBounds[static_cast<size_t>(CaveStructureType::structureName)]

void init()
{
    SET_FILL_CAVE_STRUCTURE_FUNC(CRYSTAL);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(CRYSTAL) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(OBSIDIAN_STALACTITE);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(OBSIDIAN_STALACTITE) = 5;

    SET_FILL_CAVE_STRUCTURE_FUNC(STONE_COLUMN);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(STONE_COLUMN) = 1;

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

    const uint rngSeed = SettingsManager::getWorldSeed() ^ hash(912365117u);

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
        RandomNumberGenerator rng = initRng(
            rngSeed ^ hash(static_cast<uint>(caveStructure.type)),
            caveStructure.pos_WS.x,
            caveStructure.pos_WS.y,
            caveStructure.pos_WS.z);
        fillCaveStructureFunc(
            caveStructure, ivec3(structurePosXZ_CS.x, caveStructure.pos_WS.y, structurePosXZ_CS.y /*z*/), blocks, rng);
    }
}
