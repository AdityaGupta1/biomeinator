// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "structure.h"

#include "../block.h"
#include "../chunk.h"
#include "settings_manager.h"
#include "structure_helpers.h"
#include "util/rng.h"

#include <array>
#include <glm/gtc/constants.hpp>

using namespace glm;
using namespace StructureHelpers;

#define fillStructureBlocksHeader(structureName)                                                                       \
    static void fillStructureBlocks_##structureName(                                                                   \
        const Structure& structure, ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng)

// Classic blob canopy: 5x5 ring two layers deep around the trunk top, inner cross rising two
// more, corner leaves randomized.
static void placeBlobCanopy(std::vector<Block>& blocks, ivec3 trunkTopPos_CS, RandomNumberGenerator& rng, Block leafBlock)
{
    const int baseY = trunkTopPos_CS.y - 1;
    for (int blockZ = trunkTopPos_CS.z - 2; blockZ <= trunkTopPos_CS.z + 2; ++blockZ)
    {
        for (int blockX = trunkTopPos_CS.x - 2; blockX <= trunkTopPos_CS.x + 2; ++blockX)
        {
            const ivec2 diffXZ = abs(ivec2(blockX, blockZ) - ivec2(trunkTopPos_CS.x, trunkTopPos_CS.z));
            if (diffXZ.x == 2 && diffXZ.y /*z*/ == 2)
            {
                const bool hasLeaf = rng.chance(0.5f);
                const int dy = rng.chance(0.5f) ? 1 : 0;
                const ivec3 leafPos_CS(blockX, baseY + dy, blockZ);
                if (hasLeaf && Chunk::isInChunk(leafPos_CS))
                {
                    tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(leafPos_CS)), leafBlock);
                }
            }
            else
            {
                const int leavesHeight = (diffXZ.x + diffXZ.y /*z*/ <= 1) ? 4 : 2;
                for (int dy = 0; dy < leavesHeight; ++dy)
                {
                    const ivec3 leafPos_CS(blockX, baseY + dy, blockZ);
                    if (Chunk::isInChunk(leafPos_CS))
                    {
                        tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(leafPos_CS)), leafBlock);
                    }
                }
            }
        }
    }
}

fillStructureBlocksHeader(OAK_TREE)
{
    const int trunkHeight = rng.nextInt(4, 6);
    const ivec3 trunkTopPos_CS = structurePos_CS + ivec3(0, trunkHeight, 0);
    if (Chunk::isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int y = structurePos_CS.y; y <= trunkTopPos_CS.y; ++y)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::OAK_LOG);
        }
    }

    placeBlobCanopy(blocks, trunkTopPos_CS, rng, Block::OAK_LEAVES);
}

struct BranchTip
{
    ivec3 pos_CS;
    float blobRadius;
};

// Fills the branch's log spline and records its tip for a later leaf blob. The midpoint is
// pulled down so the branch sags out of the crown before rising.
static void placeOakBranch(std::vector<Block>& blocks,
                           std::vector<BranchTip>& branchTips,
                           vec3 branchStart,
                           float angle,
                           float branchLength,
                           float branchRise,
                           float blobRadius)
{
    const vec3 branchDir(glm::cos(angle), 0.f, glm::sin(angle));
    const vec3 branchEnd = branchStart + branchDir * branchLength + vec3(0.f, branchRise, 0.f);
    const vec3 branchMid = glm::mix(branchStart, branchEnd, 0.5f) - vec3(0.f, branchRise * 0.35f, 0.f);
    const std::vector<vec3> spline = buildSpline({ branchStart, branchMid, branchEnd }, 4);
    fillSpline(blocks, spline, Block::OAK_LOG);

    branchTips.push_back({ ivec3(glm::floor(branchEnd)), blobRadius });
}

fillStructureBlocksHeader(LARGE_OAK_TREE)
{
    const int trunkHeight = rng.nextInt(7, 10);

    // 2x2 trunk with structurePos at its low corner, sunk two blocks so it seats on slopes.
    // Occasionally one of the four blocks is carved out of a mid-trunk level for texture.
    constexpr float trunkCarveChance = 0.3f;
    for (int y = -2; y <= trunkHeight; ++y)
    {
        const bool carve = rng.chance(trunkCarveChance);
        const int carvedCornerIdx = rng.nextInt(4);
        // Bare trunk only: above root tops (y <= 1), below branch attachment
        const bool inCarveRange = y >= 2 && y <= trunkHeight - 2;
        for (int cornerIdx = 0; cornerIdx < 4; ++cornerIdx)
        {
            if (carve && inCarveRange && cornerIdx == carvedCornerIdx)
            {
                continue;
            }
            const ivec3 trunkPos_CS = structurePos_CS + ivec3(cornerIdx & 1, y, cornerIdx >> 1);
            if (Chunk::isInChunk(trunkPos_CS))
            {
                tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(trunkPos_CS)), Block::OAK_LOG);
            }
        }
    }

    struct RootOffset
    {
        ivec2 posXZ;
        ivec2 outwardXZ;
    };
    constexpr RootOffset rootOffsets[] = {
        { { -1, 0 }, { -1, 0 } }, { { -1, 1 }, { -1, 0 } }, { { 2, 0 }, { 1, 0 } }, { { 2, 1 }, { 1, 0 } },
        { { 0, -1 }, { 0, -1 } }, { { 1, -1 }, { 0, -1 } }, { { 0, 2 }, { 0, 1 } }, { { 1, 2 }, { 0, 1 } },
    };
    constexpr float rootChance = 0.4f;
    constexpr float rootSpreadChance = 0.5f;
    for (const RootOffset& rootOffset : rootOffsets)
    {
        const bool hasRoot = rng.chance(rootChance);
        const int rootTopY = rng.nextInt(0, 2);
        const bool hasSpread = rng.chance(rootSpreadChance);
        if (!hasRoot)
        {
            continue;
        }

        const ivec3 rootBasePos_CS = structurePos_CS + ivec3(rootOffset.posXZ.x, -2, rootOffset.posXZ.y /*z*/);
        fillLine(blocks, rootBasePos_CS, rootBasePos_CS + ivec3(0, 2 + rootTopY, 0), Block::OAK_LOG);

        // Spread one block further out at the very bottom of the root
        if (hasSpread)
        {
            const ivec3 spreadBasePos_CS = rootBasePos_CS + ivec3(rootOffset.outwardXZ.x, 0, rootOffset.outwardXZ.y /*z*/);
            fillLine(blocks, spreadBasePos_CS, spreadBasePos_CS + ivec3(0, 2, 0), Block::OAK_LOG);
        }
    }

    const vec3 trunkTopCenter = vec3(structurePos_CS) + vec3(1.f, static_cast<float>(trunkHeight), 1.f);

    const int numBranches = rng.nextInt(3, 6);
    const float firstBranchAngle = rng.nextFloat(glm::two_pi<float>());
    constexpr float maxAngleJitterRadians = 20.f * glm::pi<float>() / 180.f;

    std::vector<BranchTip> branchTips;
    branchTips.reserve(numBranches + 2); // room for the lower branches

    // All branch logs are filled before any leaves so blobs can't block later splines
    // (tryPlaceStructureBlock is first-placed-wins), keeping branches connected to the trunk
    for (int i = 0; i < numBranches; ++i)
    {
        const float angle =
            firstBranchAngle + (i / static_cast<float>(numBranches)) * glm::two_pi<float>() + rng.nextFloatAbs(maxAngleJitterRadians);
        const float branchLength = rng.nextFloat(2.5f, 5.5f);
        const float branchRise = rng.nextFloat(0.5f, 3.5f);
        const vec3 branchStart = trunkTopCenter - vec3(0.f, rng.nextFloat(2.f), 0.f);
        const float blobRadius = rng.nextFloat(2.2f, 3.f);
        placeOakBranch(blocks, branchTips, branchStart, angle, branchLength, branchRise, blobRadius);
    }

    // One or two shorter branches lower on the trunk so the foliage isn't all at the crown
    const int numLowerBranches = rng.nextInt(1, 3);
    for (int i = 0; i < numLowerBranches; ++i)
    {
        const float angle = rng.nextFloat(glm::two_pi<float>());
        const float branchLength = rng.nextFloat(2.f, 4.f);
        const float branchRise = rng.nextFloat(1.f, 2.f);
        const float startHeight = rng.nextFloat(0.45f, 0.7f) * trunkHeight;
        const vec3 branchStart = vec3(structurePos_CS) + vec3(1.f, startHeight, 1.f);
        const float blobRadius = rng.nextFloat(1.8f, 2.4f);
        placeOakBranch(blocks, branchTips, branchStart, angle, branchLength, branchRise, blobRadius);
    }

    placeLeafBlob(blocks, ivec3(glm::floor(trunkTopCenter)), 3.f, rng, Block::OAK_LEAVES);
    for (const BranchTip& branchTip : branchTips)
    {
        placeLeafBlob(blocks, branchTip.pos_CS, branchTip.blobRadius, rng, Block::OAK_LEAVES);
    }
}

fillStructureBlocksHeader(BIRCH_TREE)
{
    const float colorRoll = rng.nextFloat();
    const Block leafBlock = (colorRoll < 0.7f)    ? Block::BIRCH_LEAVES_GREEN
                            : (colorRoll < 0.95f) ? Block::BIRCH_LEAVES_YELLOW
                                                  : Block::BIRCH_LEAVES_ORANGE;

    const int trunkHeight = rng.nextInt(7, 11);
    const ivec3 trunkTopPos_CS = structurePos_CS + ivec3(0, trunkHeight, 0);
    if (Chunk::isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int y = structurePos_CS.y; y <= trunkTopPos_CS.y; ++y)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::BIRCH_LOG);
        }
    }

    constexpr float sideLogChance = 0.5f;
    const bool hasSideLog = rng.chance(sideLogChance);
    const int sideLogY = rng.nextInt(2, trunkHeight - 1);
    const ivec2 sideLogOffset = neighborOffset(static_cast<NeighborDirection>(rng.nextInt(4)));
    if (hasSideLog)
    {
        const ivec3 sideLogPos_CS = structurePos_CS + ivec3(sideLogOffset.x, sideLogY, sideLogOffset.y /*z*/);
        if (Chunk::isInChunkXZ(sideLogPos_CS))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(sideLogPos_CS), Block::BIRCH_LOG);
        }
    }

    constexpr float trunkBlobChance = 0.5f;
    const bool hasBlob = rng.chance(trunkBlobChance);
    const ivec2 blobOffset = neighborOffset(static_cast<NeighborDirection>(rng.nextInt(4)));
    // The blob (2 tall) must sit at least 4 blocks off the ground and keep at least one air row
    // below the canopy base at trunkHeight - 1; short trees can't satisfy both and get no blob
    const int maxBlobY = trunkHeight - 4;
    if (hasBlob && maxBlobY >= 4)
    {
        const int blobY = rng.nextInt(4, maxBlobY + 1);
        placeLeafCap(blocks, structurePos_CS + ivec3(blobOffset.x, blobY, blobOffset.y /*z*/), 1.f, 2.f, 2.f, rng, leafBlock);
    }

    placeBlobCanopy(blocks, trunkTopPos_CS, rng, leafBlock);
}

fillStructureBlocksHeader(SAGUARO_CACTUS)
{
    const int trunkHeight = rng.nextInt(4, 10);

    if (Chunk::isInChunkXZ(structurePos_CS))
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
        if (Chunk::isInChunkXZ(armConnectorPos_CS))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(armConnectorPos_CS), Block::CACTUS);
        }

        const ivec3 armBendPos_CS = armConnectorPos_CS + ivec3(dirOffset.x, 0, dirOffset.y /*z*/);
        if (Chunk::isInChunkXZ(armBendPos_CS))
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
    std::vector<vec3> ctrlPts;
    ctrlPts.push_back(structurePos_CS);
    for (int i = 0; i < 2; ++i)
    {
        ctrlPts.push_back(ctrlPts.back() + vec3(rng.nextFloatAbs(3), rng.nextFloat(4, 7), rng.nextFloatAbs(3)));
    }

    const std::vector<vec3> spline = buildSpline(ctrlPts, 3);
    fillSpline(blocks, spline, Block::PALM_LOG);

    const vec3 trunkTip = spline.back();
    const vec3 trunkDir = glm::normalize(trunkTip - spline[spline.size() - 2]);

    constexpr vec3 worldUp(0.f, 1.f, 0.f);
    const vec3 ref = (glm::abs(glm::dot(trunkDir, worldUp)) < 0.9f) ? worldUp : vec3(1.f, 0.f, 0.f);
    const vec3 basis1 = glm::normalize(glm::cross(ref, trunkDir));
    const vec3 basis2 = glm::normalize(glm::cross(trunkDir, basis1));

    const ivec3 trunkTipPos_CS = ivec3(glm::floor(trunkTip));
    if (Chunk::isInChunkXZ(trunkTipPos_CS))
    {
        Block& trunkTipBlock = blocks[Chunk::blockPosToIdx(trunkTipPos_CS)];
        if (trunkTipBlock == Block::PALM_LOG)
        {
            trunkTipBlock = Block::PALM_LEAVES;
        }
    }

    constexpr float maxAngleJitterRadians = 5.0f * glm::pi<float>() / 180.0f;
    const int numLeaves = rng.nextInt(7, 11);

    for (int i = 0; i < numLeaves; ++i)
    {
        const float baseAngle = (i / static_cast<float>(numLeaves)) * glm::two_pi<float>();
        const float angle = baseAngle + rng.nextFloatAbs(maxAngleJitterRadians);
        const vec3 leafDir = glm::cos(angle) * basis1 + glm::sin(angle) * basis2;

        const float segment1Length = rng.nextFloat(3.f, 4.f);
        const float segment2Length = rng.nextFloat(2.f, 3.f);

        const vec3 segment1End = trunkTip + leafDir * segment1Length;
        vec3 segment2End = segment1End + glm::normalize(leafDir * segment2Length - glm::vec3(0.f, 1.8f, 0.f)) * segment2Length;

        fillLine(blocks, ivec3(glm::floor(trunkTip)), ivec3(glm::floor(segment1End)), Block::PALM_LEAVES);
        fillLine(blocks, ivec3(glm::floor(segment1End)), ivec3(glm::floor(segment2End)), Block::PALM_LEAVES);
    }
}

fillStructureBlocksHeader(ACACIA_TREE)
{
    const int trunkBaseHeight = (int)(3.5f + 2.5f * rng.nextFloat());
    vec3 trunkTopPos = structurePos_CS;
    trunkTopPos.y += trunkBaseHeight;
    if (Chunk::isInChunkXZ(structurePos_CS))
    {
        uint blockIdx = Chunk::blockPosToIdx(structurePos_CS);
        for (int dy = 0; dy <= trunkBaseHeight; ++dy)
        {
            tryPlaceStructureBlock(blocks, blockIdx++, Block::ACACIA_LOG);
        }
    }

    const float branchAngle = rng.nextFloat(glm::two_pi<float>());
    const vec3 primaryBranchDir(glm::cos(branchAngle), 0.f, glm::sin(branchAngle));
    const vec3 primaryBranchStart = trunkTopPos;
    vec3 primaryBranchEnd = primaryBranchStart + primaryBranchDir * rng.nextFloat(3.5f, 4.5f);
    primaryBranchEnd.y += rng.nextFloat(4.5f, 5.5f);

    fillLine(blocks, glm::floor(primaryBranchStart), glm::floor(primaryBranchEnd), Block::ACACIA_LOG);
    placeLeafCap(blocks, glm::floor(primaryBranchEnd), 2.5f, 4.5f, 2.f, rng, Block::ACACIA_LEAVES);

    if (!rng.chance(0.5f))
    {
        return;
    }

    const float secondaryBranchAngle = branchAngle + rng.nextFloat(glm::half_pi<float>(), glm::three_over_two_pi<float>());
    const vec3 secondaryBranchDir(glm::cos(secondaryBranchAngle), 0.f, glm::sin(secondaryBranchAngle));
    vec3 secondaryBranchStart = trunkTopPos;
    secondaryBranchStart.y -= rng.nextFloat(0.8f, 1.6f);
    vec3 secondaryBranchEnd = secondaryBranchStart + secondaryBranchDir * rng.nextFloat(2.5f, 3.5f);
    secondaryBranchEnd.y += rng.nextFloat(3.f, 4.f);

    fillLine(blocks, glm::floor(secondaryBranchStart), glm::floor(secondaryBranchEnd), Block::ACACIA_LOG);
    placeLeafCap(blocks, secondaryBranchEnd, 2.f, 4.f, 2.f, rng, Block::ACACIA_LEAVES);
}

StructureGen::StructureGen(StructureType type, uint32_t gridCellSideLength, uint32_t gridCellPadding, uint32_t flags)
    : StructureGen(std::vector<StructureGenVariant>{ { type } }, gridCellSideLength, gridCellPadding, flags)
{}

StructureGen::StructureGen(std::vector<StructureGenVariant> variants,
                           uint32_t gridCellSideLength,
                           uint32_t gridCellPadding,
                           uint32_t flags)
    : variants(std::move(variants)), gridCellSideLength(gridCellSideLength), gridCellPadding(gridCellPadding),
      flags(flags)
{}

StructureType StructureGen::pickVariant(RandomNumberGenerator& rng) const
{
    float totalWeight = 0.f;
    for (const StructureGenVariant& variant : variants)
    {
        totalWeight += variant.weight;
    }

    float roll = rng.nextFloat(totalWeight);
    for (const StructureGenVariant& variant : variants)
    {
        roll -= variant.weight;
        if (roll < 0.f)
        {
            return variant.type;
        }
    }
    return variants.back().type;
}

uint32_t StructureGen::gridSalt() const
{
    uint32_t salt = 0;
    for (const StructureGenVariant& variant : variants)
    {
        salt = hash(salt ^ static_cast<uint32_t>(variant.type));
    }
    return salt;
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
    STRUCTURE_BOUNDS_BY_NAME(PALM_TREE) = 12;

    SET_FILL_STRUCTURE_FUNC(ACACIA_TREE);
    STRUCTURE_BOUNDS_BY_NAME(ACACIA_TREE) = 12;

    SET_FILL_STRUCTURE_FUNC(LARGE_OAK_TREE);
    STRUCTURE_BOUNDS_BY_NAME(LARGE_OAK_TREE) = 10;

    SET_FILL_STRUCTURE_FUNC(BIRCH_TREE);
    STRUCTURE_BOUNDS_BY_NAME(BIRCH_TREE) = 3;

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

        if (structureAabbRejectsChunk(structureMinXZ_CS, structureMaxXZ_CS))
        {
            continue;
        }

        const FillStructureFunc fillStructureFunc = fillStructureFuncs[static_cast<size_t>(structure.type)];
        RandomNumberGenerator rng = initRng(rngSeed ^ hash(static_cast<uint>(structure.type)), structure.pos_WS.x, structure.pos_WS.y, structure.pos_WS.z);
        fillStructureFunc(structure, ivec3(structurePosXZ_CS.x, structure.pos_WS.y, structurePosXZ_CS.y /*z*/), blocks, rng);
    }
}
