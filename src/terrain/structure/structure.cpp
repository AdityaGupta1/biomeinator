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
                    tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(leafPos_CS)), leafBlock, false /*canReplaceWater*/);
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
                        tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(leafPos_CS)), leafBlock, false /*canReplaceWater*/);
                    }
                }
            }
        }
    }
}

static void placeTreeLeaf(std::vector<Block>& blocks, ivec3 pos_CS, Block leafBlock)
{
    if (Chunk::isInChunk(pos_CS))
    {
        tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(pos_CS)), leafBlock, false /*canReplaceWater*/);
    }
}

static void placeRoundLeafLayer(std::vector<Block>& blocks, ivec3 centerPos_CS, float radius, Block leafBlock)
{
    // A radius from k*sqrt(2) up to k + 1 fills a full square (3x3, 5x5), which looks blocky.
    // Rounding down to just below k*sqrt(2) keeps the layer round.
    for (int k = 1; k <= 2; ++k)
    {
        const float squareRadius = k * glm::root_two<float>();
        if (radius >= squareRadius && radius < k + 1)
        {
            radius = squareRadius - 0.01f;
        }
    }

    const int radiusCeil = static_cast<int>(glm::ceil(radius));
    for (int dz = -radiusCeil; dz <= radiusCeil; ++dz)
    {
        for (int dx = -radiusCeil; dx <= radiusCeil; ++dx)
        {
            const float distance = length(vec2(dx, dz));
            if (distance > radius)
            {
                continue;
            }
            placeTreeLeaf(blocks, centerPos_CS + ivec3(dx, 0, dz), leafBlock);
        }
    }
}

// Unlike fillLine, consecutive blocks always share a face, so a diagonal branch reads as one solid
// limb rather than a staircase of blocks touching only at their edges
static void fillFaceConnectedLine(std::vector<Block>& blocks, vec3 start, vec3 end, Block block)
{
    const auto place = [&](ivec3 pos_CS)
    {
        if (Chunk::isInChunk(pos_CS))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(pos_CS)), block);
        }
    };

    ivec3 pos_CS(glm::floor(start));
    place(pos_CS);
    const int numSteps = static_cast<int>(glm::ceil(length(end - start) * 4.f));
    for (int i = 1; i <= numSteps; ++i)
    {
        const ivec3 nextPos_CS(glm::floor(mix(start, end, i / static_cast<float>(numSteps))));
        for (int axis = 0; axis < 3; ++axis)
        {
            if (nextPos_CS[axis] != pos_CS[axis])
            {
                pos_CS[axis] = nextPos_CS[axis];
                place(pos_CS);
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

// A rounded clump of leaf layers, thickest just above its center: distinct from its neighbors,
// unlike overlapping spheres, which merge into one disc
static void placeOakLeafClump(std::vector<Block>& blocks, ivec3 centerPos_CS, float radius)
{
    const std::array<std::pair<int, float>, 6> layers{{ { -2, radius - 1.6f }, { -1, radius - 0.6f }, { 0, radius },
                                                         { 1, radius - 0.3f }, { 2, radius - 1.1f },
                                                         { 3, radius - 2.2f } }};
    for (const auto& [dy, layerRadius] : layers)
    {
        if (layerRadius >= 1.f)
        {
            placeRoundLeafLayer(blocks, centerPos_CS + ivec3(0, dy, 0), layerRadius, Block::OAK_LEAVES);
        }
    }
}

struct LargeOakShape
{
    int minTrunkHeight;
    int maxTrunkHeight; // exclusive
    int minBranches;
    int maxBranches; // exclusive
    float minBranchLength;
    float maxBranchLength;
    float minBranchRise;
    float maxBranchRise;
    float minClumpRadius;
    float maxClumpRadius;
    int minLowerBranches;
    int maxLowerBranches; // exclusive
    float minLowerBranchLength;
    float maxLowerBranchLength;
    float minLowerBranchRise;
    float maxLowerBranchRise;
    float minLowerClumpRadius;
    float maxLowerClumpRadius;
    float crownClumpRadius;
};

static void fillLargeOakTree(ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng,
                             const LargeOakShape& shape)
{
    const int trunkHeight = rng.nextInt(shape.minTrunkHeight, shape.maxTrunkHeight);

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

    // The 2x2 trunk's center line, which branches leave from
    const vec3 trunkCenter = vec3(structurePos_CS) + vec3(1.f, 0.f, 1.f);

    struct Clump
    {
        ivec3 pos_CS;
        float radius;
    };
    std::vector<Clump> clumps;

    // Straight limbs angling up and out, each ending in its own clump, with a smaller clump partway
    // along that hides the limb's stair-stepping. All logs go in before any leaves
    // (tryPlaceStructureBlock is first-placed-wins), so clumps can't cut a later limb off.
    const auto placeBranch = [&](float startY, float angle, float length, float rise, float clumpRadius)
    {
        const vec3 start = trunkCenter + vec3(0.f, startY, 0.f);
        const vec3 end = start + vec3(glm::cos(angle) * length, rise, glm::sin(angle) * length);
        fillFaceConnectedLine(blocks, start, end, Block::OAK_LOG);
        clumps.push_back({ ivec3(glm::floor(end)), clumpRadius });
        clumps.push_back({ ivec3(glm::floor(mix(start, end, 0.6f))), clumpRadius * 0.7f });
    };

    const int numBranches = rng.nextInt(shape.minBranches, shape.maxBranches);
    const float firstBranchAngle = rng.nextFloat(glm::two_pi<float>());
    constexpr float maxAngleJitterRadians = 20.f * glm::pi<float>() / 180.f;
    for (int i = 0; i < numBranches; ++i)
    {
        const float angle =
            firstBranchAngle + (i / static_cast<float>(numBranches)) * glm::two_pi<float>() + rng.nextFloatAbs(maxAngleJitterRadians);
        placeBranch(trunkHeight - rng.nextFloat(3.f), angle, rng.nextFloat(shape.minBranchLength, shape.maxBranchLength),
                    rng.nextFloat(shape.minBranchRise, shape.maxBranchRise),
                    rng.nextFloat(shape.minClumpRadius, shape.maxClumpRadius));
    }

    // Shorter limbs lower on the trunk give the crown depth instead of a single lid
    const int numLowerBranches = rng.nextInt(shape.minLowerBranches, shape.maxLowerBranches);
    const float firstLowerBranchAngle = rng.nextFloat(glm::two_pi<float>());
    for (int i = 0; i < numLowerBranches; ++i)
    {
        const float angle = firstLowerBranchAngle + (i / static_cast<float>(numLowerBranches)) * glm::two_pi<float>() +
                            rng.nextFloatAbs(maxAngleJitterRadians);
        placeBranch(rng.nextFloat(0.45f, 0.7f) * trunkHeight, angle,
                    rng.nextFloat(shape.minLowerBranchLength, shape.maxLowerBranchLength),
                    rng.nextFloat(shape.minLowerBranchRise, shape.maxLowerBranchRise),
                    rng.nextFloat(shape.minLowerClumpRadius, shape.maxLowerClumpRadius));
    }

    clumps.push_back({ ivec3(glm::floor(trunkCenter)) + ivec3(0, trunkHeight + 1, 0), shape.crownClumpRadius });
    for (const Clump& clump : clumps)
    {
        placeOakLeafClump(blocks, clump.pos_CS, clump.radius);
    }
}

fillStructureBlocksHeader(LARGE_OAK_TREE)
{
    fillLargeOakTree(structurePos_CS, blocks, rng,
                     { .minTrunkHeight = 6, .maxTrunkHeight = 9, .minBranches = 3, .maxBranches = 5,
                       .minBranchLength = 3.f, .maxBranchLength = 5.f, .minBranchRise = 2.f, .maxBranchRise = 4.f,
                       .minClumpRadius = 2.6f, .maxClumpRadius = 3.2f, .minLowerBranches = 2, .maxLowerBranches = 4,
                       .minLowerBranchLength = 2.5f, .maxLowerBranchLength = 4.f, .minLowerBranchRise = 1.f,
                       .maxLowerBranchRise = 2.f, .minLowerClumpRadius = 2.2f, .maxLowerClumpRadius = 2.7f,
                       .crownClumpRadius = 3.f });
}

// A rare old giant with a broad, spreading crown that rises above the old-growth canopy
fillStructureBlocksHeader(GIANT_OAK_TREE)
{
    fillLargeOakTree(structurePos_CS, blocks, rng,
                     { .minTrunkHeight = 12, .maxTrunkHeight = 17, .minBranches = 5, .maxBranches = 8,
                       .minBranchLength = 5.f, .maxBranchLength = 8.f, .minBranchRise = 3.f, .maxBranchRise = 6.f,
                       .minClumpRadius = 3.3f, .maxClumpRadius = 4.f, .minLowerBranches = 3, .maxLowerBranches = 6,
                       .minLowerBranchLength = 4.f, .maxLowerBranchLength = 6.f, .minLowerBranchRise = 1.5f,
                       .maxLowerBranchRise = 3.f, .minLowerClumpRadius = 2.9f, .maxLowerClumpRadius = 3.5f,
                       .crownClumpRadius = 4.f });
}

struct BirchShape
{
    int minTrunkHeight;
    int maxTrunkHeight; // exclusive
    // Leaf colors are chosen per tree: green below greenChance, else yellow below yellowChance, else orange
    float greenChance;
    float yellowChance;
    // Extra leaf layers down the upper trunk, so a tall trunk doesn't end in a tiny cap
    bool hasTallCrown{ false };
};

static void fillBirchTree(ivec3 structurePos_CS, std::vector<Block>& blocks, RandomNumberGenerator& rng,
                          const BirchShape& shape)
{
    const float colorRoll = rng.nextFloat();
    const Block leafBlock = (colorRoll < shape.greenChance)    ? Block::BIRCH_LEAVES_GREEN
                            : (colorRoll < shape.yellowChance) ? Block::BIRCH_LEAVES_YELLOW
                                                               : Block::BIRCH_LEAVES_ORANGE;

    const int trunkHeight = rng.nextInt(shape.minTrunkHeight, shape.maxTrunkHeight);
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

    if (shape.hasTallCrown)
    {
        // A second, 2-tall cap under the canopy, joined to it by a narrow plus layer instead of
        // bare trunk, and sometimes trailed by another plus below
        constexpr float plusRadius = 1.2f;
        placeLeafCap(blocks, trunkTopPos_CS - ivec3(0, 4, 0), 2.f, 2.5f, 2.f, rng, leafBlock);
        placeRoundLeafLayer(blocks, trunkTopPos_CS - ivec3(0, 2, 0), plusRadius, leafBlock);
        if (rng.chance(0.5f))
        {
            placeRoundLeafLayer(blocks, trunkTopPos_CS - ivec3(0, 5, 0), plusRadius, leafBlock);
        }
    }
    placeBlobCanopy(blocks, trunkTopPos_CS, rng, leafBlock);
}

fillStructureBlocksHeader(BIRCH_TREE)
{
    fillBirchTree(structurePos_CS, blocks, rng,
                  { .minTrunkHeight = 7, .maxTrunkHeight = 11, .greenChance = 0.7f, .yellowChance = 0.95f });
}

// Taiga birches turn yellow but never orange
fillStructureBlocksHeader(BOREAL_BIRCH_TREE)
{
    fillBirchTree(structurePos_CS, blocks, rng,
                  { .minTrunkHeight = 7, .maxTrunkHeight = 11, .greenChance = 0.75f, .yellowChance = 1.f });
}

fillStructureBlocksHeader(AUTUMN_BIRCH_TREE)
{
    fillBirchTree(structurePos_CS, blocks, rng,
                  { .minTrunkHeight = 7, .maxTrunkHeight = 11, .greenChance = 0.45f, .yellowChance = 0.8f });
}

fillStructureBlocksHeader(TALL_AUTUMN_BIRCH_TREE)
{
    fillBirchTree(structurePos_CS, blocks, rng,
                  { .minTrunkHeight = 12, .maxTrunkHeight = 17, .greenChance = 0.45f, .yellowChance = 0.8f,
                    .hasTallCrown = true });
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

fillStructureBlocksHeader(CYPRESS_TREE)
{
    const ivec2 chunkPosXZ_WS =
        ivec2(structure.pos_WS.x, structure.pos_WS.z) - ivec2(structurePos_CS.x, structurePos_CS.z);
    const uint worldSeed = SettingsManager::getWorldSeed();

    const float trunkHeight = rng.nextFloat(24.f, 35.f);
    const int trunkTopY = static_cast<int>(trunkHeight);

    // The knee and moss scans below index columns by Y without per-block chunk-bounds checks
    ASSERT(structurePos_CS.y + trunkTopY + 5 < static_cast<int>(chunkSizeY), "cypress tree extends past the world top");

    // Trunk radius flares into a wide buttress at the base (sunk two blocks and rooted down to
    // local ground so it seats on slopes) and tapers quickly above it; noise wobble, faded out
    // above the lower trunk, makes the buttress fluted instead of round
    constexpr int maxRootDepth = 10;
    for (int y = -2; y <= trunkTopY; ++y)
    {
        const float trunkRatio = (y + 2.f) / (trunkHeight + 2.f);
        const float flare = 0.73f + trunkRatio;
        const float baseRadius = 0.5f * (1.3f + trunkRatio) / (flare * flare * flare * flare) + 0.5f;
        const DiscWobble wobble{
            .strength = 0.3f * (1.f - glm::smoothstep(0.15f, 0.55f, trunkRatio)),
            .frequency = 0.15f,
            .seed = worldSeed ^ hash(602149583),
        };

        // Only the bottom layer roots, seating the buttress rim on local ground
        placeWobbledDisc(blocks, structurePos_CS + ivec3(0, y, 0), chunkPosXZ_WS, baseRadius, wobble,
                         Block::CYPRESS_LOG, -1 /*rootStepY*/, (y == -2) ? maxRootDepth : 0);
    }

    // Knees: short log stubs ringing the trunk, seated on local ground found by scanning the
    // already-generated column
    const int numKnees = rng.nextInt(6, 13);
    for (int i = 0; i < numKnees; ++i)
    {
        const float kneeAngle = rng.nextFloat(glm::two_pi<float>());
        const float kneeDistance = rng.nextFloat(3.f, 8.f);
        const int kneeHeight = rng.nextInt(1, 3);

        const int kneeX_CS = structurePos_CS.x + static_cast<int>(glm::round(glm::cos(kneeAngle) * kneeDistance));
        const int kneeZ_CS = structurePos_CS.z + static_cast<int>(glm::round(glm::sin(kneeAngle) * kneeDistance));
        if (!Chunk::isInChunkXZ(ivec3(kneeX_CS, 0, kneeZ_CS)))
        {
            continue;
        }

        int groundY = -1;
        for (int y = structurePos_CS.y + 2; y >= glm::max(structurePos_CS.y - 6, 0); --y)
        {
            const Block block = blocks[Chunk::blockPosToIdx(uvec3(kneeX_CS, y, kneeZ_CS))];
            if (block == Block::AIR || block == Block::WATER || block == Block::WATER_TOP)
            {
                continue;
            }
            if (block == Block::GRASS_BLOCK || block == Block::DIRT || block == Block::MUD)
            {
                groundY = y;
            }
            break;
        }
        if (groundY == -1)
        {
            continue;
        }

        for (int y = groundY + 1; y <= groundY + kneeHeight; ++y)
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(kneeX_CS, y, kneeZ_CS)), Block::CYPRESS_LOG);
        }
    }

    // All branch wood is filled before any leaf caps so caps can't block the lines
    // (tryPlaceStructureBlock is first-placed-wins), keeping branches connected to the trunk
    const int numBranches = rng.nextInt(6, 11);
    float branchHeight = trunkHeight - 1.f;
    float branchAngle = rng.nextFloat(glm::two_pi<float>());

    std::vector<vec3> branchTips;
    branchTips.reserve(numBranches);
    for (int i = 0; i < numBranches; ++i)
    {
        branchHeight -= rng.nextFloat(1.f, 4.6f);
        if (branchHeight < 5.f)
        {
            break;
        }
        branchAngle += glm::half_pi<float>() + rng.nextFloat(glm::pi<float>());

        vec3 branchEnd(glm::cos(branchAngle), 0.f, glm::sin(branchAngle));
        branchEnd *= rng.nextFloat(4.f, 5.5f);
        branchEnd.y = rng.nextFloat(2.2f, 3.4f);
        // Branches shrink toward the crown
        branchEnd *= 1.f - 0.3f * (branchHeight / trunkHeight);

        const vec3 branchStart = vec3(structurePos_CS) + vec3(0.f, branchHeight, 0.f);
        branchEnd += branchStart;

        fillLine(blocks, ivec3(glm::floor(branchStart)), ivec3(glm::floor(branchEnd)), Block::CYPRESS_LOG);
        branchTips.push_back(branchEnd);
    }

    constexpr float leavesDroopChance = 0.2f;
    placeLeafCap(blocks, structurePos_CS + ivec3(0, trunkTopY, 0), 3.f, 4.5f, 2.f, rng, Block::CYPRESS_LEAVES);
    for (const vec3& branchTip : branchTips)
    {
        placeLeafCap(blocks, ivec3(glm::floor(branchTip)), 2.5f, 4.f, 2.f, rng, Block::CYPRESS_LEAVES,
                     leavesDroopChance, chunkPosXZ_WS);
    }

    // Spanish moss: strands hanging below leaf blocks that have air underneath, more likely on the
    // lower caps. Chance and length come from a position-hashed RNG rather than the structure
    // stream. The strand's bottom block is always the tip, even when water or terrain cuts the
    // strand short.
    constexpr float mossBaseChance = 0.45f;
    const StructureBounds& mossBounds = Structures::getStructureBounds(structure.type);
    for (int dz = mossBounds.minDiffXZ.y /*z*/; dz <= mossBounds.maxDiffXZ.y /*z*/; ++dz)
    {
        for (int dx = mossBounds.minDiffXZ.x; dx <= mossBounds.maxDiffXZ.x; ++dx)
        {
            const int x_CS = structurePos_CS.x + dx;
            const int z_CS = structurePos_CS.z + dz;
            if (!Chunk::isInChunkXZ(ivec3(x_CS, 0, z_CS)))
            {
                continue;
            }

            for (int y = glm::max(structurePos_CS.y, 1); y <= structurePos_CS.y + trunkTopY + 5; ++y)
            {
                if (blocks[Chunk::blockPosToIdx(uvec3(x_CS, y, z_CS))] != Block::CYPRESS_LEAVES ||
                    blocks[Chunk::blockPosToIdx(uvec3(x_CS, y - 1, z_CS))] != Block::AIR)
                {
                    continue;
                }

                const float mossChance =
                    mossBaseChance * (1.f - glm::smoothstep(0.f, trunkHeight, static_cast<float>(y - structurePos_CS.y)));
                RandomNumberGenerator mossRng =
                    initRng(worldSeed ^ hash(812930471), static_cast<uint32_t>(chunkPosXZ_WS.x + x_CS),
                            static_cast<uint32_t>(chunkPosXZ_WS.y /*z*/ + z_CS), static_cast<uint32_t>(y));
                if (!mossRng.chance(mossChance))
                {
                    continue;
                }
                const int strandLength = mossRng.nextInt(1, 4);

                uint lastMossIdx = 0;
                int numPlaced = 0;
                for (int i = 1; i <= strandLength; ++i)
                {
                    const int strandY = y - i;
                    if (strandY < 0)
                    {
                        break;
                    }
                    const uint blockIdx = Chunk::blockPosToIdx(uvec3(x_CS, strandY, z_CS));
                    if (blocks[blockIdx] != Block::AIR)
                    {
                        break;
                    }
                    blocks[blockIdx] = Block::SPANISH_MOSS;
                    lastMossIdx = blockIdx;
                    ++numPlaced;
                }
                if (numPlaced > 0)
                {
                    blocks[lastMossIdx] = Block::SPANISH_MOSS_TIP;
                }
            }
        }
    }
}

static void fillConiferTrunk(std::vector<Block>& blocks, ivec3 rootPos_CS, int height, Block logBlock)
{
    for (int y = 0; y <= height; ++y)
    {
        const ivec3 pos = rootPos_CS + ivec3(0, y, 0);
        if (!Chunk::isInChunk(pos))
        {
            continue;
        }
        const uint blockIdx = Chunk::blockPosToIdx(uvec3(pos));
        // Conifers on different cliff ledges or in dense stands can have overlapping crowns.
        // Their leaves must not interrupt another tree's trunk; rock stays untouched.
        const BlockData& existingData = Blocks::getBlockData(blocks[blockIdx]);
        if (existingData.shape == BlockShape::CUBE && existingData.translucent)
        {
            blocks[blockIdx] = logBlock;
        }
        else
        {
            tryPlaceStructureBlock(blocks, blockIdx, logBlock);
        }
    }
}

// A flat foliage pad: a rounded disc with a plus-shaped layer on top
static void placePinePad(std::vector<Block>& blocks, ivec3 centerPos_CS, float radius)
{
    placeRoundLeafLayer(blocks, centerPos_CS, radius, Block::PINE_LEAVES);
    placeRoundLeafLayer(blocks, centerPos_CS + ivec3(0, 1, 0), 1.2f, Block::PINE_LEAVES);
}

struct PadCrownShape
{
    int topY;
    int minPadY;
    int minSidePads;
    int maxSidePads; // exclusive
    float minBranchLength;
    float maxBranchLength;
    float minPadRadius;
    float maxPadRadius;
    float topPadRadius;
};

// Foliage in separate flat pads held out on branches over a bare trunk, so the crown's outline is
// irregular and flat-topped rather than one rounded mass. Side pads step down from just below the
// trunk top; a pad also caps the trunk.
static void placePadCrown(std::vector<Block>& blocks, ivec3 structurePos_CS, const PadCrownShape& shape, RandomNumberGenerator& rng)
{
    struct Pad
    {
        ivec3 pos_CS;
        float radius;
    };
    std::vector<Pad> pads;
    const int numSidePads = rng.nextInt(shape.minSidePads, shape.maxSidePads);
    pads.reserve(numSidePads + 1);
    const float firstPadAngle = rng.nextFloat(glm::two_pi<float>());
    // Golden-angle steps never put two pads directly opposite, which reads as a symmetric pair of arms
    constexpr float padAngleStepRadians = 137.5f * glm::pi<float>() / 180.f;
    constexpr float maxAngleJitterRadians = 20.f * glm::pi<float>() / 180.f;
    const vec3 trunkCenter = vec3(structurePos_CS) + vec3(0.5f, 0.f, 0.5f);
    int padY = shape.topY - 2;
    for (int i = 0; i < numSidePads; ++i)
    {
        const float angle = firstPadAngle + i * padAngleStepRadians + rng.nextFloatAbs(maxAngleJitterRadians);
        // Lower branches reach further out, widening the crown toward its base
        const float reach = numSidePads > 1 ? static_cast<float>(i) / (numSidePads - 1) : 0.f;
        const float branchLength = mix(shape.minBranchLength, shape.maxBranchLength, reach) + rng.nextFloatAbs(0.3f);
        const vec3 tip = trunkCenter + vec3(branchLength * glm::cos(angle), padY + 0.5f, branchLength * glm::sin(angle));
        fillFaceConnectedLine(blocks, trunkCenter + vec3(0.f, padY - 0.5f, 0.f), tip, Block::PINE_LOG);
        pads.push_back({ ivec3(glm::floor(tip)), rng.nextFloat(shape.minPadRadius, shape.maxPadRadius) });
        padY = max(padY - rng.nextInt(1, 3), shape.minPadY);
    }
    pads.push_back({ structurePos_CS + ivec3(0, shape.topY, 0), shape.topPadRadius });

    // Pads go in after every branch so their leaves can't block a later branch's logs
    for (const Pad& pad : pads)
    {
        placePinePad(blocks, pad.pos_CS, pad.radius);
    }
}

fillStructureBlocksHeader(PINE_TREE)
{
    const int height = rng.nextInt(5, 8);
    fillConiferTrunk(blocks, structurePos_CS, height, Block::PINE_LOG);
    placePadCrown(blocks, structurePos_CS,
                  { .topY = height, .minPadY = height / 2, .minSidePads = 2, .maxSidePads = 4,
                    .minBranchLength = 1.5f, .maxBranchLength = 2.2f, .minPadRadius = 2.f, .maxPadRadius = 2.4f,
                    .topPadRadius = 2.2f },
                  rng);
}

// A tall Scots pine: a long bare trunk with the crown only in its upper part
fillStructureBlocksHeader(BOREAL_PINE_TREE)
{
    const int height = rng.nextInt(12, 17);
    fillConiferTrunk(blocks, structurePos_CS, height, Block::PINE_LOG);
    placePadCrown(blocks, structurePos_CS,
                  { .topY = height, .minPadY = height * 3 / 5, .minSidePads = 3, .maxSidePads = 6,
                    .minBranchLength = 1.5f, .maxBranchLength = 3.f, .minPadRadius = 1.8f, .maxPadRadius = 2.3f,
                    .topPadRadius = 2.5f },
                  rng);
}

fillStructureBlocksHeader(PINE_SHRUB)
{
    const int height = rng.nextInt(2, 4);
    fillConiferTrunk(blocks, structurePos_CS, height, Block::PINE_LOG);
    placeBlobCanopy(blocks, structurePos_CS + ivec3(0, height, 0), rng, Block::PINE_LEAVES);
}

fillStructureBlocksHeader(FIR_TREE)
{
    const int height = rng.nextInt(10, 19);
    const int trunkTopY = height - 1;
    const int crownBottomY = rng.nextInt(2, 4);
    const bool isTall = height >= 14;
    const int maxRadius = isTall ? 3 : 2;
    const int maxTopSkips = isTall ? 2 : 1;
    fillConiferTrunk(blocks, structurePos_CS, trunkTopY, Block::FIR_LOG);

    // Layers from the top down, like a vanilla spruce: a leaf capping the trunk, plus-shaped layers
    // alternating with bare trunk, then whorls that widen in a sawtooth (1-2, 1-2-3, ...). Each layer
    // is a square with its corners cut; a radius of 0 below the cap is just trunk.
    int radius = 0;
    int cycleMaxRadius = 1;
    int numTopSkips = 0;
    for (int y = trunkTopY + 1; y >= crownBottomY; --y)
    {
        for (int dz = -radius; dz <= radius; ++dz)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                if (radius > 0 && abs(dx) == radius && abs(dz) == radius)
                {
                    continue;
                }
                placeTreeLeaf(blocks, structurePos_CS + ivec3(dx, y, dz), Block::FIR_LEAVES);
            }
        }

        if (radius < cycleMaxRadius)
        {
            ++radius;
        }
        else if (numTopSkips < maxTopSkips)
        {
            radius = 0;
            ++numTopSkips;
        }
        else
        {
            // Leaving the plus-shaped top steps straight out to radius 2; later cycles restart at 1
            radius = (radius == 1) ? 2 : 1;
            cycleMaxRadius = min(cycleMaxRadius + 1, maxRadius);
        }
    }
}

// A round leaf layer with an irregular outline: stretched and shifted a little off the trunk, its
// rim thinned out, and a few sprigs poking out past it. Every leaf must touch the layer's solid
// core through a face, so thinning never strands a sprig in midair. All draws depend only on the
// layer's shape, never on chunk clipping, so every chunk produces the same layer.
static void placeRaggedLeafLayer(std::vector<Block>& blocks, ivec3 centerPos_CS, float radius,
                                 RandomNumberGenerator& rng, Block leafBlock)
{
    const vec2 stretch(rng.nextFloat(0.85f, 1.15f), rng.nextFloat(0.85f, 1.15f));
    const vec2 offset(rng.nextFloatAbs(0.4f), rng.nextFloatAbs(0.4f));
    const int reach = static_cast<int>(glm::ceil(radius * 1.15f + 1.5f));
    const int side = 2 * reach + 1;

    enum class Cell : uint8_t
    {
        EMPTY,
        CORE,
        RIM,
        SPRIG,
    };
    std::vector<Cell> cells(side * side, Cell::EMPTY);
    const auto cellAt = [&](int dx, int dz) -> Cell&
    {
        return cells[(dx + reach) + (dz + reach) * side];
    };
    for (int dz = -reach; dz <= reach; ++dz)
    {
        for (int dx = -reach; dx <= reach; ++dx)
        {
            const float distance = length((vec2(dx, dz) - offset) / stretch);
            const float roll = rng.nextFloat();
            if (distance <= radius - 1.f || (dx == 0 && dz == 0))
            {
                cellAt(dx, dz) = Cell::CORE;
            }
            else if (distance <= radius)
            {
                cellAt(dx, dz) = roll < 0.25f ? Cell::EMPTY : Cell::RIM;
            }
            else if (distance <= radius + 1.f && roll < 0.12f)
            {
                cellAt(dx, dz) = Cell::SPRIG;
            }
        }
    }

    // Rim cells hang off the core and sprigs off a rim cell, each through a face
    const auto hasNeighbor = [&](int dx, int dz, Cell neighbor)
    {
        const std::array<ivec2, 4> faceOffsets{{ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }};
        for (const ivec2 faceOffset : faceOffsets)
        {
            const int nx = dx + faceOffset.x;
            const int nz = dz + faceOffset.y;
            if (glm::abs(nx) <= reach && glm::abs(nz) <= reach && cellAt(nx, nz) == neighbor)
            {
                return true;
            }
        }
        return false;
    };
    // Rim cells off the core are dropped before sprigs look for a rim cell to hang from
    for (const Cell attachedCell : { Cell::RIM, Cell::SPRIG })
    {
        const Cell anchor = attachedCell == Cell::RIM ? Cell::CORE : Cell::RIM;
        for (int dz = -reach; dz <= reach; ++dz)
        {
            for (int dx = -reach; dx <= reach; ++dx)
            {
                if (cellAt(dx, dz) == attachedCell && !hasNeighbor(dx, dz, anchor))
                {
                    cellAt(dx, dz) = Cell::EMPTY;
                }
            }
        }
    }
    for (int dz = -reach; dz <= reach; ++dz)
    {
        for (int dx = -reach; dx <= reach; ++dx)
        {
            if (cellAt(dx, dz) != Cell::EMPTY)
            {
                placeTreeLeaf(blocks, centerPos_CS + ivec3(dx, 0, dz), leafBlock);
            }
        }
    }
}

// A towering narrow cone: a thick trunk with buttress roots, bare for its lower part, under a
// crown of whorls that widen in a sawtooth down to the crown's base
fillStructureBlocksHeader(REDWOOD_TREE)
{
    const int height = rng.nextInt(39, 56);
    const int crownBottomY = static_cast<int>(height * rng.nextFloat(0.25f, 0.3f));
    const float maxCrownRadius = mix(6.f, 7.f, static_cast<float>(height - 39) / 16.f);

    const auto placeLog = [&](ivec3 offset)
    {
        const ivec3 pos_CS = structurePos_CS + offset;
        if (Chunk::isInChunk(pos_CS))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(uvec3(pos_CS)), Block::REDWOOD_LOG);
        }
    };

    // Buttress roots: lobes on the flare that are widest at the ground and taper smoothly back
    // into the trunk as they rise, so they never stand apart from it
    struct Root
    {
        vec2 dir;
        float reach;
        int height;
    };
    const int numRoots = rng.nextInt(4, 7);
    const float firstRootAngle = rng.nextFloat(glm::two_pi<float>());
    std::vector<Root> roots;
    roots.reserve(numRoots);
    for (int i = 0; i < numRoots; ++i)
    {
        const float angle = firstRootAngle + (i / static_cast<float>(numRoots)) * glm::two_pi<float>() +
                            rng.nextFloatAbs(0.3f);
        roots.push_back({ vec2(glm::cos(angle), glm::sin(angle)), rng.nextFloat(1.2f, 2.f), rng.nextInt(4, 9) });
    }

    // Round trunk tapering to one block wide, flaring over its bottom few blocks, sunk so the
    // flare seats on slopes
    constexpr int maxRootReach = 2;
    for (int y = -3; y < height; ++y)
    {
        const float t = static_cast<float>(max(y, 0)) / height;
        const float flare = max(1.f - max(y, 0) / 5.f, 0.f);
        const float radius = mix(1.5f, 0.6f, t) + 1.2f * flare * flare;
        const int radiusCeil = static_cast<int>(glm::ceil(radius)) + maxRootReach;
        for (int dz = -radiusCeil; dz <= radiusCeil; ++dz)
        {
            for (int dx = -radiusCeil; dx <= radiusCeil; ++dx)
            {
                const vec2 offsetXZ(dx, dz);
                const float distance = length(offsetXZ);
                float columnRadius = radius;
                for (const Root& root : roots)
                {
                    const float rootFalloff = max(1.f - max(y, 0) / static_cast<float>(root.height), 0.f);
                    const float alignment = distance > 0.f ? max(dot(offsetXZ / distance, root.dir), 0.f) : 0.f;
                    columnRadius = max(columnRadius, radius + root.reach * rootFalloff * rootFalloff *
                                                                  glm::pow(alignment, 6.f));
                }
                if (distance <= columnRadius)
                {
                    placeLog(ivec3(dx, y, dz));
                }
            }
        }
    }

    // From the top down: a leaf capping the trunk, a short spire of plus layers with one gap of
    // bare trunk, then whorls whose sawtooth widens toward the crown's base. Each whorl starts at the
    // crown's envelope and narrows over the layers above it.
    placeTreeLeaf(blocks, structurePos_CS + ivec3(0, height, 0), Block::REDWOOD_LEAVES);
    constexpr int spireLength = 3;
    placeRoundLeafLayer(blocks, structurePos_CS + ivec3(0, height - 1, 0), 1.2f, Block::REDWOOD_LEAVES);
    placeRoundLeafLayer(blocks, structurePos_CS + ivec3(0, height - 3, 0), 1.2f, Block::REDWOOD_LEAVES);
    int whorlTopY = height - spireLength - 1;
    while (whorlTopY >= crownBottomY)
    {
        const int whorlLength = rng.nextInt(2, 4);
        const int whorlBottomY = max(whorlTopY - whorlLength + 1, crownBottomY);
        const float t = static_cast<float>(height - spireLength - whorlBottomY) / (height - spireLength - crownBottomY);
        const float envelope = mix(1.8f, maxCrownRadius, glm::pow(t, 0.85f)) + rng.nextFloatAbs(0.6f);
        for (int y = whorlBottomY; y <= whorlTopY; ++y)
        {
            const float radius = max(envelope - 1.1f * (y - whorlBottomY), 1.2f);
            placeRaggedLeafLayer(blocks, structurePos_CS + ivec3(0, y, 0), radius, rng, Block::REDWOOD_LEAVES);
        }
        whorlTopY = whorlBottomY - 1;
    }
}

fillStructureBlocksHeader(CHERRY_TREE)
{
    const ivec2 chunkPosXZ_WS =
        ivec2(structure.pos_WS.x, structure.pos_WS.z) - ivec2(structurePos_CS.x, structurePos_CS.z);
    const Block leafBlock = rng.chance(0.2f) ? Block::CHERRY_LEAVES_WHITE : Block::CHERRY_LEAVES_PINK;

    // Sunk a block into the ground so the bare trunk stays short
    const ivec3 trunkBasePos_CS = structurePos_CS - ivec3(0, 1, 0);
    const int trunkHeight = rng.nextInt(3, 6);
    fillLine(blocks, trunkBasePos_CS, trunkBasePos_CS + ivec3(0, trunkHeight, 0), Block::CHERRY_LOG);

    // The trunk forks into a few branches angling up and out, each carrying its own broad canopy
    const int numBranches = rng.nextInt(2, 4);
    const float firstBranchAngle = rng.nextFloat(glm::two_pi<float>());
    constexpr float maxAngleJitterRadians = 25.f * glm::pi<float>() / 180.f;
    const vec3 trunkTopCenter = vec3(trunkBasePos_CS) + vec3(0.5f, static_cast<float>(trunkHeight) + 0.5f, 0.5f);

    struct Canopy
    {
        ivec3 pos_CS;
        float radius;
    };
    std::vector<Canopy> canopies;
    canopies.reserve(numBranches);

    // All branch logs are filled before any leaves so canopies can't block later branches
    for (int i = 0; i < numBranches; ++i)
    {
        const float angle =
            firstBranchAngle + (i / static_cast<float>(numBranches)) * glm::two_pi<float>() + rng.nextFloatAbs(maxAngleJitterRadians);
        const float branchLength = rng.nextFloat(3.f, 5.f);
        const float branchRise = rng.nextFloat(2.f, 3.5f);
        const vec3 branchEnd = trunkTopCenter + vec3(glm::cos(angle) * branchLength, branchRise, glm::sin(angle) * branchLength);
        fillFaceConnectedLine(blocks, trunkTopCenter, branchEnd, Block::CHERRY_LOG);
        canopies.push_back({ ivec3(glm::floor(branchEnd)), rng.nextFloat(3.8f, 4.8f) });
    }

    constexpr float canopyDroopChance = 0.5f;
    for (const Canopy& canopy : canopies)
    {
        // A wide, low dome resting on the branch end, so the branch stays visible beneath it
        placeLeafCap(blocks, canopy.pos_CS, 1.5f, canopy.radius, 3.f, rng, leafBlock,
                     canopyDroopChance, chunkPosXZ_WS);
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
    STRUCTURE_BOUNDS_BY_NAME(PALM_TREE) = 12;

    SET_FILL_STRUCTURE_FUNC(ACACIA_TREE);
    STRUCTURE_BOUNDS_BY_NAME(ACACIA_TREE) = 12;

    SET_FILL_STRUCTURE_FUNC(LARGE_OAK_TREE);
    STRUCTURE_BOUNDS_BY_NAME(LARGE_OAK_TREE) = 10;

    SET_FILL_STRUCTURE_FUNC(BIRCH_TREE);
    STRUCTURE_BOUNDS_BY_NAME(BIRCH_TREE) = 3;

    SET_FILL_STRUCTURE_FUNC(CYPRESS_TREE);
    STRUCTURE_BOUNDS_BY_NAME(CYPRESS_TREE) = 11;

    SET_FILL_STRUCTURE_FUNC(PINE_TREE);
    STRUCTURE_BOUNDS_BY_NAME(PINE_TREE) = 5;
    SET_FILL_STRUCTURE_FUNC(PINE_SHRUB);
    STRUCTURE_BOUNDS_BY_NAME(PINE_SHRUB) = 2;

    SET_FILL_STRUCTURE_FUNC(FIR_TREE);
    STRUCTURE_BOUNDS_BY_NAME(FIR_TREE) = 5;

    SET_FILL_STRUCTURE_FUNC(CHERRY_TREE);
    STRUCTURE_BOUNDS_BY_NAME(CHERRY_TREE) = 11;

    SET_FILL_STRUCTURE_FUNC(BOREAL_PINE_TREE);
    STRUCTURE_BOUNDS_BY_NAME(BOREAL_PINE_TREE) = 7;

    SET_FILL_STRUCTURE_FUNC(BOREAL_BIRCH_TREE);
    STRUCTURE_BOUNDS_BY_NAME(BOREAL_BIRCH_TREE) = 3;

    SET_FILL_STRUCTURE_FUNC(AUTUMN_BIRCH_TREE);
    STRUCTURE_BOUNDS_BY_NAME(AUTUMN_BIRCH_TREE) = 3;
    SET_FILL_STRUCTURE_FUNC(TALL_AUTUMN_BIRCH_TREE);
    STRUCTURE_BOUNDS_BY_NAME(TALL_AUTUMN_BIRCH_TREE) = 3;

    SET_FILL_STRUCTURE_FUNC(GIANT_OAK_TREE);
    STRUCTURE_BOUNDS_BY_NAME(GIANT_OAK_TREE) = 14;

    SET_FILL_STRUCTURE_FUNC(REDWOOD_TREE);
    STRUCTURE_BOUNDS_BY_NAME(REDWOOD_TREE) = 11;

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
