// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "cave_structure.h"

#include "../block.h"
#include "../cave_biome.h"
#include "../chunk.h"
#include "settings_manager.h"
#include "structure_helpers.h"
#include "util/rng.h"

#include <algorithm>
#include <array>
#include <glm/gtc/constants.hpp>

using namespace glm;
using namespace StructureHelpers;

static uint32_t columnBlockIdx(ivec2 colPosXZ_CS, int y)
{
    return Chunk::blockPosToIdx(uvec3(colPosXZ_CS.x, y, colPosXZ_CS.y /*z*/));
}

static Block columnBlockAt(const std::vector<Block>& blocks, ivec2 colPosXZ_CS, int y)
{
    return blocks[columnBlockIdx(colPosXZ_CS, y)];
}

// Seeded from the anchor position so every chunk this structure overlaps rolls the same values
static RandomNumberGenerator initStructureRng(const CaveStructure& structure, uint32_t salt)
{
    return initRng(SettingsManager::getWorldSeed() ^ hash(salt),
                   static_cast<uint32_t>(structure.pos_WS.x),
                   static_cast<uint32_t>(structure.pos_WS.y),
                   static_cast<uint32_t>(structure.pos_WS.z));
}

// Visits every in-chunk column of a disc around the anchor whose radius is rolled per structure in
// [minRadius, maxRadius]. A structure is filled once by every chunk it overlaps and each fill sees
// only its own columns, so each column gets an RNG seeded from its world position (plus anchor y)
// rather than one stream advanced across the footprint, which would desynchronize between chunks.
// func(ivec2 colPosXZ_CS, float distFromCenter, int radius, RandomNumberGenerator& columnRng)
template <typename PerColumnFunc>
static void forEachDiscColumn(const CaveStructure& structure,
                              ivec3 structurePos_CS,
                              int minRadius,
                              int maxRadius,
                              uint32_t radiusSalt,
                              uint32_t columnSalt,
                              PerColumnFunc&& func)
{
    const uint worldSeed = SettingsManager::getWorldSeed();
    const int radius = initStructureRng(structure, radiusSalt).nextInt(minRadius, maxRadius + 1);

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

            RandomNumberGenerator columnRng = initRng(worldSeed ^ hash(columnSalt),
                                                      static_cast<uint32_t>(structure.pos_WS.x + dx),
                                                      static_cast<uint32_t>(structure.pos_WS.z + dz),
                                                      static_cast<uint32_t>(structure.pos_WS.y));
            func(colPosXZ_CS, distFromCenter, radius, columnRng);
        }
    }
}

static bool isSolidCubeBlock(Block block)
{
    const BlockData& blockData = Blocks::getBlockData(block);
    return blockData.type == BlockType::SOLID && blockData.shape == BlockShape::CUBE;
}

#define fillCaveStructureBlocksHeader(structureName)                                                                   \
    static void fillCaveStructureBlocks_##structureName(                                                               \
        const Chunk& chunk, const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks)

fillCaveStructureBlocksHeader(STONE_COLUMN)
{
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const ivec2 colPosXZ_CS(structurePos_CS.x + dx, structurePos_CS.z + dz);
            if (!Chunk::isInChunkXZ(colPosXZ_CS))
            {
                continue;
            }

            const int maxY = std::min(structurePos_CS.y + structure.availableHeight, static_cast<int>(chunkSizeY));
            for (int y = structurePos_CS.y; y < maxY; ++y)
            {
                tryPlaceStructureBlock(blocks, columnBlockIdx(colPosXZ_CS, y), Block::SCALESTONE);
            }
        }
    }
}

// The mound at the anchor: a stack of discs tapering to a peak
inline constexpr int crystalMoundMinHeight = 3;
inline constexpr int crystalMoundMaxHeight = 6;
inline constexpr float crystalMoundMinBaseRadius = 2.5f;
inline constexpr float crystalMoundMaxBaseRadius = 4.f;
inline constexpr float crystalMoundWobbleStrength = 0.4f;
inline constexpr float crystalMoundWobbleFrequency = 0.3f;
// Second wobble octave: broad lumps alone read as a smooth blob, so a finer one roughens the
// silhouette at block scale
inline constexpr float crystalMoundWobbleDetailFrequency = 2.7f;
inline constexpr float crystalMoundWobbleDetailStrength = 0.5f;
// How far the base ring reaches down for local ground, so a mound on uneven floor isn't undercut
inline constexpr int crystalMoundMaxRootDepth = 3;
// Air blocks left between the peak and the far side of the pocket
inline constexpr int crystalMoundTipGap = 2;

// The knees: short WHITE_CRYSTAL columns standing straight off the surface around the mound
inline constexpr int crystalKneeMinCount = 12;
inline constexpr int crystalKneeMaxCount = 20;
inline constexpr float crystalKneeMinDist = 2.f;
inline constexpr float crystalKneeMaxDist = 9.f;
inline constexpr int crystalKneeMinHeight = 2;
inline constexpr int crystalKneeMaxHeight = 4;
inline constexpr int crystalKneeSurfaceSearchDist = 6;
// Widest a cluster reaches in XZ: the outermost knee, the mound being far narrower
inline constexpr int crystalClusterMaxReachXZ = 10;

// Natural cave rock a knee may stand on. The scan that finds it also sees blocks written by
// structures filled earlier and by this fill's own mound, so the accepted set is a whitelist rather
// than "anything solid" (see knowledge/terrain/structure_system.md).
static bool isCrystalKneeGroundBlock(Block block)
{
    return block == Block::BASALT || block == Block::CRACKED_BASALT || block == Block::STONE;
}

// Discs from the anchor to the peak, each narrower than the last, with the base ring rooted to
// local ground.
static void placeCrystalMound(std::vector<Block>& blocks,
                              ivec3 anchorPos_CS,
                              ivec2 chunkPosXZ_WS,
                              int height,
                              float baseRadius,
                              int yGrowDir)
{
    const uint worldSeed = SettingsManager::getWorldSeed();
    const DiscWobble wobble{
        .strength = crystalMoundWobbleStrength,
        .frequency = crystalMoundWobbleFrequency,
        .seed = worldSeed ^ hash(1902384571u),
        .detailStrength = crystalMoundWobbleDetailStrength,
        .detailFrequencyMultiplier = crystalMoundWobbleDetailFrequency,
        .detailSeed = worldSeed ^ hash(3310277119u),
    };

    for (int layer = 0; layer < height; ++layer)
    {
        const float layerRadius = baseRadius * (1.f - static_cast<float>(layer) / height);
        const ivec3 layerCenterPos_CS(anchorPos_CS.x, anchorPos_CS.y + yGrowDir * layer, anchorPos_CS.z);
        placeWobbledDisc(blocks, layerCenterPos_CS, chunkPosXZ_WS, layerRadius, wobble, Block::CRYSTAL_CORE,
                         -yGrowDir /*rootStepY*/, (layer == 0) ? crystalMoundMaxRootDepth : 0);
    }
}

// Finds the surface a knee stands on by scanning this column back along the grow direction from a
// little past the anchor, as cypress knees do. Draws no RNG (see the stream invariant in
// knowledge/terrain/structure_system.md).
static bool findCrystalKneeSurfaceY(
    const std::vector<Block>& blocks, ivec2 colPosXZ_CS, int anchorY, int yGrowDir, int& outSurfaceY)
{
    for (int i = -2; i <= crystalKneeSurfaceSearchDist; ++i)
    {
        const int y = anchorY - yGrowDir * i;
        if (y < 0 || y >= static_cast<int>(chunkSizeY))
        {
            continue;
        }

        const Block block = columnBlockAt(blocks, colPosXZ_CS, y);
        if (block == Block::AIR)
        {
            continue;
        }
        if (!isCrystalKneeGroundBlock(block))
        {
            return false;
        }

        outSurfaceY = y;
        return true;
    }
    return false;
}

// A small mountain of bare CRYSTAL_CORE on a cave floor or ceiling, ringed by short WHITE_CRYSTAL
// columns standing on the surface around it the way cypress knees ring a trunk. Keeping the emitter
// and the glass apart is what makes a cluster interesting to light: the mound lights the cave
// directly, and whichever knees stand in its path scatter and tint it.
//
// yGrowDir is +1 for a cluster on a floor and -1 for one hanging from a ceiling; the two are exact
// mirrors, so the whole shape is written in terms of it.
static void fillCrystalCluster(
    const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks, int yGrowDir)
{
    RandomNumberGenerator rng = initStructureRng(structure, 1174509823);

    // The gen's minLayerHeight is what keeps this range non-empty
    const int maxHeight = std::min(structure.availableHeight - crystalMoundTipGap, crystalMoundMaxHeight);
    const int height = rng.nextInt(crystalMoundMinHeight, std::max(maxHeight, crystalMoundMinHeight) + 1);
    const float baseRadius = rng.nextFloat(crystalMoundMinBaseRadius, crystalMoundMaxBaseRadius);
    const ivec2 chunkPosXZ_WS =
        ivec2(structure.pos_WS.x, structure.pos_WS.z) - ivec2(structurePos_CS.x, structurePos_CS.z);
    placeCrystalMound(blocks, structurePos_CS, chunkPosXZ_WS, height, baseRadius, yGrowDir);

    const int numKnees = rng.nextInt(crystalKneeMinCount, crystalKneeMaxCount + 1);
    for (int i = 0; i < numKnees; ++i)
    {
        // Every knee's parameters are drawn before its surface scan and its chunk-bounds check, so
        // each chunk this cluster overlaps consumes the same RNG stream
        const float angle = rng.nextFloat(glm::two_pi<float>());
        const float dist = rng.nextFloat(crystalKneeMinDist, crystalKneeMaxDist);
        const int kneeHeight = rng.nextInt(crystalKneeMinHeight, crystalKneeMaxHeight + 1);

        const vec2 outwardXZ(glm::cos(angle), glm::sin(angle));
        const ivec2 kneePosXZ_CS =
            ivec2(structurePos_CS.x, structurePos_CS.z) + ivec2(glm::round(outwardXZ * dist));
        if (!Chunk::isInChunkXZ(kneePosXZ_CS))
        {
            continue;
        }

        int surfaceY;
        if (!findCrystalKneeSurfaceY(blocks, kneePosXZ_CS, structurePos_CS.y, yGrowDir, surfaceY))
        {
            continue;
        }

        for (int i = 1; i <= kneeHeight; ++i)
        {
            const int y = surfaceY + yGrowDir * i;
            if (y < 0 || y >= static_cast<int>(chunkSizeY))
            {
                break;
            }
            tryPlaceStructureBlock(blocks, columnBlockIdx(kneePosXZ_CS, y), Block::WHITE_CRYSTAL);
        }
    }
}

fillCaveStructureBlocksHeader(CRYSTAL_CLUSTER)
{
    fillCrystalCluster(structure, structurePos_CS, blocks, 1);
}

fillCaveStructureBlocksHeader(CRYSTAL_CLUSTER_HANGING)
{
    fillCrystalCluster(structure, structurePos_CS, blocks, -1);
}

inline constexpr int lampClusterMaxRadius = 4;
inline constexpr int lampClusterMaxDepth = 6;
inline constexpr int lampClusterMaxRise = 3;
inline constexpr int lampClusterGrowthAttempts = 600;

// Grown like Minecraft's nether glowstone: one block under the ceiling anchor, then random
// attempts in a box around it that only place where the terrain is air and the cell touches
// exactly one existing cluster cell. The single-neighbor rule produces spidery strings with gaps
// instead of a blob, and since every cell is added touching the cluster, nothing ever floats.
// Growth may rise above the anchor row so the cluster climbs a ceiling that pulls away upward
// rather than sticking out flat beneath it. Terrain is read through the chunk's neighborhood
// air mask, so every chunk the cluster overlaps grows the identical shape. Growth ignores
// structure blocks placed earlier (surface structures), so a cell landing on one is skipped at
// write time; only reachable near cave mouths, accepted.
fillCaveStructureBlocksHeader(LAMP_CLUSTER)
{
    constexpr int gridSideXZ = 2 * lampClusterMaxRadius + 1;
    constexpr int gridHeight = lampClusterMaxDepth + lampClusterMaxRise + 1;
    // Local grid coords: x, z in [0, gridSideXZ), y in [0, gridHeight) with the anchor row at lampClusterMaxDepth
    std::array<bool, gridSideXZ * gridHeight * gridSideXZ> cells{};
    const auto cellIdx = [](int x, int y, int z)
    {
        return x + gridSideXZ * (y + gridHeight * z);
    };
    const auto isCell = [&](int x, int y, int z)
    {
        if (x < 0 || x >= gridSideXZ || z < 0 || z >= gridSideXZ || y < 0 || y >= gridHeight)
        {
            return false;
        }
        return cells[cellIdx(x, y, z)];
    };
    const auto gridToWorld = [&](int x, int y, int z)
    {
        return structure.pos_WS + ivec3(x - lampClusterMaxRadius, y - lampClusterMaxDepth, z - lampClusterMaxRadius);
    };

    RandomNumberGenerator rng = initStructureRng(structure, 1830294761);

    cells[cellIdx(lampClusterMaxRadius, lampClusterMaxDepth, lampClusterMaxRadius)] = true;
    for (int attempt = 0; attempt < lampClusterGrowthAttempts; ++attempt)
    {
        // Difference of two uniform draws biases attempts toward the anchor
        const int x = lampClusterMaxRadius + rng.nextInt(lampClusterMaxRadius + 1) - rng.nextInt(lampClusterMaxRadius + 1);
        const int z = lampClusterMaxRadius + rng.nextInt(lampClusterMaxRadius + 1) - rng.nextInt(lampClusterMaxRadius + 1);
        const int y = lampClusterMaxDepth + rng.nextInt(lampClusterMaxRise + 1) - rng.nextInt(lampClusterMaxDepth + 1);
        if (cells[cellIdx(x, y, z)] || !chunk.isTerrainAir_WS(gridToWorld(x, y, z)))
        {
            continue;
        }

        const int numNeighbors = static_cast<int>(isCell(x - 1, y, z)) + static_cast<int>(isCell(x + 1, y, z)) +
                                 static_cast<int>(isCell(x, y - 1, z)) + static_cast<int>(isCell(x, y + 1, z)) +
                                 static_cast<int>(isCell(x, y, z - 1)) + static_cast<int>(isCell(x, y, z + 1));
        if (numNeighbors == 1)
        {
            cells[cellIdx(x, y, z)] = true;
        }
    }

    for (int z = 0; z < gridSideXZ; ++z)
    {
        for (int x = 0; x < gridSideXZ; ++x)
        {
            const ivec2 colPosXZ_CS(structurePos_CS.x + x - lampClusterMaxRadius, structurePos_CS.z + z - lampClusterMaxRadius);
            if (!Chunk::isInChunkXZ(colPosXZ_CS))
            {
                continue;
            }

            for (int y = 0; y < gridHeight; ++y)
            {
                if (!cells[cellIdx(x, y, z)])
                {
                    continue;
                }
                const int y_CS = structurePos_CS.y + y - lampClusterMaxDepth;
                if (y_CS < 0 || y_CS >= static_cast<int>(chunkSizeY))
                {
                    continue;
                }
                tryPlaceStructureBlock(blocks, columnBlockIdx(colPosXZ_CS, y_CS), Block::LAMP);
            }
        }
    }
}

inline constexpr int mossPinkClusterMinRadius = 2;
inline constexpr int mossPinkClusterMaxRadius = 4;
inline constexpr int mossPinkClusterFloorSearchDist = 3;
inline constexpr float mossPinkClusterBloomChance = 0.35f;
inline constexpr float mossPinkClusterCenterBudChance = 0.55f;
inline constexpr float mossPinkClusterEdgeBudChance = 0.2f;

// Finds the air block sitting on a cave-flora ground block near the anchor y in this column.
static bool findCaveFloraStandY(const std::vector<Block>& blocks, ivec2 colPosXZ_CS, int anchorY, int& outStandY)
{
    const int maxY = static_cast<int>(chunkSizeY) - 1;
    for (int dy = 0; dy <= mossPinkClusterFloorSearchDist; ++dy)
    {
        for (const int y : { anchorY - dy, anchorY + dy })
        {
            if (y < 1 || y > maxY)
            {
                continue;
            }
            if (columnBlockAt(blocks, colPosXZ_CS, y) == Block::AIR &&
                CaveBiomes::isCaveFloraGroundBlock(columnBlockAt(blocks, colPosXZ_CS, y - 1)))
            {
                outStandY = y;
                return true;
            }
        }
    }
    return false;
}

// Blooms at the center, buds scattered around them with chance falling off toward the edge.
// Only stands on cave-flora ground (moss / overgrown rock).
fillCaveStructureBlocksHeader(MOSS_PINK_CLUSTER)
{
    forEachDiscColumn(
        structure, structurePos_CS, mossPinkClusterMinRadius, mossPinkClusterMaxRadius, 1122334455, 2011223344,
        [&](ivec2 colPosXZ_CS, float distFromCenter, int radius, RandomNumberGenerator& rng)
        {
            const bool isCenter = distFromCenter <= 1.f;
            const float chance = isCenter
                ? mossPinkClusterBloomChance
                : glm::mix(mossPinkClusterCenterBudChance, mossPinkClusterEdgeBudChance, distFromCenter / radius);
            if (!rng.chance(chance))
            {
                return;
            }

            int standY;
            if (!findCaveFloraStandY(blocks, colPosXZ_CS, structurePos_CS.y, standY))
            {
                return;
            }
            tryPlaceStructureBlock(blocks, columnBlockIdx(colPosXZ_CS, standY), isCenter ? Block::MOSS_PINK_BLOOM : Block::MOSS_PINK_BUD);
        });
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

// Only a non-emissive solid cube can anchor a strand: not an X-shaped block (e.g. another
// cluster's vine) and not a lamp.
static bool isCaveVinesCeilingBlock(Block block)
{
    return isSolidCubeBlock(block) && !Blocks::getBlockData(block).markAsEmitter;
}

// Finds the topmost air block under this column's ceiling by searching from the anchor y.
static bool findCaveVinesStrandStartY(const std::vector<Block>& blocks, ivec2 colPosXZ_CS, int anchorY, int& outStartY)
{
    const int maxY = static_cast<int>(chunkSizeY) - 1;

    if (columnBlockAt(blocks, colPosXZ_CS, anchorY) == Block::AIR)
    {
        for (int y = anchorY; y < anchorY + caveVinesCeilingSearchDist && y < maxY; ++y)
        {
            const Block above = columnBlockAt(blocks, colPosXZ_CS, y + 1);
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
        if (columnBlockAt(blocks, colPosXZ_CS, y) == Block::AIR)
        {
            if (!isCaveVinesCeilingBlock(columnBlockAt(blocks, colPosXZ_CS, y + 1)))
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
    // Leave at least one air block below the longest strand
    const int maxStrandHeight = std::min(caveVinesMaxStrandHeight, structure.availableHeight - 1);

    forEachDiscColumn(
        structure, structurePos_CS, caveVinesMinRadius, caveVinesMaxRadius, 2093481127, 1497203641,
        [&](ivec2 colPosXZ_CS, float distFromCenter, int radius, RandomNumberGenerator& rng)
        {
            const float strandChance =
                glm::mix(caveVinesCenterStrandChance, caveVinesEdgeStrandChance, distFromCenter / radius);
            if (!rng.chance(strandChance))
            {
                return;
            }

            const int strandHeight = rng.nextInt(caveVinesMinStrandHeight, maxStrandHeight + 1);
            int strandStartY;
            if (!findCaveVinesStrandStartY(blocks, colPosXZ_CS, structurePos_CS.y, strandStartY))
            {
                return;
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
                const uint32_t blockIdx = columnBlockIdx(colPosXZ_CS, y);
                if (blocks[blockIdx] != Block::AIR || columnBlockAt(blocks, colPosXZ_CS, y - 1) != Block::AIR)
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
        });
}

namespace CaveStructures
{

using FillCaveStructureFunc = void (*)(const Chunk& chunk, const CaveStructure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks);
static std::array<FillCaveStructureFunc, static_cast<size_t>(CaveStructureType::COUNT)> fillCaveStructureFuncs{};

#define FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) fillCaveStructureFuncs[static_cast<size_t>(CaveStructureType::structureName)]
#define SET_FILL_CAVE_STRUCTURE_FUNC(structureName) FILL_CAVE_STRUCTURE_FUNC_BY_NAME(structureName) = fillCaveStructureBlocks_##structureName;

static std::array<StructureBounds, static_cast<size_t>(CaveStructureType::COUNT)> caveStructureBounds{};

#define CAVE_STRUCTURE_BOUNDS_BY_NAME(structureName) caveStructureBounds[static_cast<size_t>(CaveStructureType::structureName)]

void init()
{
    SET_FILL_CAVE_STRUCTURE_FUNC(LAMP_CLUSTER);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(LAMP_CLUSTER) = lampClusterMaxRadius;

    SET_FILL_CAVE_STRUCTURE_FUNC(STONE_COLUMN);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(STONE_COLUMN) = 1;

    SET_FILL_CAVE_STRUCTURE_FUNC(CRYSTAL_CLUSTER);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(CRYSTAL_CLUSTER) = crystalClusterMaxReachXZ;

    SET_FILL_CAVE_STRUCTURE_FUNC(CRYSTAL_CLUSTER_HANGING);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(CRYSTAL_CLUSTER_HANGING) = crystalClusterMaxReachXZ;

    SET_FILL_CAVE_STRUCTURE_FUNC(MOSS_PINK_CLUSTER);
    CAVE_STRUCTURE_BOUNDS_BY_NAME(MOSS_PINK_CLUSTER) = mossPinkClusterMaxRadius;

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

void Chunk::fillCaveStructureBlocks(const CaveStructure* caveStructures, uint32_t numCaveStructures, CaveStructureType type)
{
    const ivec2 chunkPosBlocksXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    for (uint32_t i = 0; i < numCaveStructures; ++i)
    {
        const CaveStructure& caveStructure = caveStructures[i];
        if (caveStructure.type != type)
        {
            continue;
        }

        const ivec2 structurePosXZ_CS = ivec2(caveStructure.pos_WS.x, caveStructure.pos_WS.z) - chunkPosBlocksXZ_WS;
        const StructureBounds& bounds = CaveStructures::getCaveStructureBounds(caveStructure.type);
        const ivec2 structureMinXZ_CS = structurePosXZ_CS + bounds.minDiffXZ;
        const ivec2 structureMaxXZ_CS = structurePosXZ_CS + bounds.maxDiffXZ;

        if (structureAabbRejectsChunk(structureMinXZ_CS, structureMaxXZ_CS))
        {
            continue;
        }

        const FillCaveStructureFunc fillCaveStructureFunc = fillCaveStructureFuncs[static_cast<size_t>(caveStructure.type)];
        fillCaveStructureFunc(*this, caveStructure, ivec3(structurePosXZ_CS.x, caveStructure.pos_WS.y, structurePosXZ_CS.y /*z*/), blocks);
    }
}
