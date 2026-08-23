// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"
#include "../chunk.h"
#include "settings_manager.h"
#include "util/rng.h"

#include <vector>

namespace StructureHelpers
{

inline void tryPlaceStructureBlock(std::vector<Block>& blocks, uint32_t blockIdx, Block newBlock, bool canReplaceWater = true)
{
    Block& block = blocks[blockIdx];
    if (block == Block::AIR || (canReplaceWater && (block == Block::WATER || block == Block::WATER_TOP)))
    {
        block = newBlock;
    }
}

// Trilinear value noise in [-1, 1]; interpolating between position-seeded corner values keeps
// neighboring blocks correlated, unlike a per-block RNG
inline float valueNoise3(glm::vec3 pos, uint32_t seed)
{
    const glm::vec3 posFloor = glm::floor(pos);
    const glm::vec3 posFract = pos - posFloor;
    const glm::vec3 t = posFract * posFract * (3.f - 2.f * posFract);
    const glm::ivec3 basePos = glm::ivec3(posFloor);

    float result = 0.f;
    for (int cornerIdx = 0; cornerIdx < 8; ++cornerIdx)
    {
        const glm::ivec3 corner(cornerIdx & 1, (cornerIdx >> 1) & 1, cornerIdx >> 2);
        const glm::ivec3 cornerPos = basePos + corner;
        RandomNumberGenerator cornerRng = initRng(
            seed, static_cast<uint32_t>(cornerPos.x), static_cast<uint32_t>(cornerPos.y), static_cast<uint32_t>(cornerPos.z));
        const glm::vec3 weights = glm::mix(glm::vec3(1.f) - t, t, glm::vec3(corner));
        result += weights.x * weights.y * weights.z * (cornerRng.nextFloat() * 2.f - 1.f);
    }
    return result;
}

// True when a structure's XZ AABB (chunk space) lies wholly outside this chunk, so none of its
// blocks land here. Lets the per-chunk fill passes skip structures that only overhang neighbors.
inline bool structureAabbRejectsChunk(glm::ivec2 minXZ_CS, glm::ivec2 maxXZ_CS)
{
    return minXZ_CS.x >= static_cast<int>(chunkSizeXZ) || minXZ_CS.y /*z*/ >= static_cast<int>(chunkSizeXZ) ||
           maxXZ_CS.x < 0 || maxXZ_CS.y /*z*/ < 0;
}

inline void fillLine(std::vector<Block>& blocks, glm::ivec3 startPos_CS, glm::ivec3 endPos_CS, Block block)
{
    const glm::ivec3 minPt = glm::min(startPos_CS, endPos_CS);
    const glm::ivec3 maxPt = glm::max(startPos_CS, endPos_CS);
    const glm::ivec3 chunkSize(chunkSizeXZ, chunkSizeY, chunkSizeXZ);
    if (glm::any(glm::lessThan(maxPt, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(minPt, chunkSize)))
    {
        return;
    }

    const glm::ivec3 d = glm::abs(endPos_CS - startPos_CS);
    const glm::ivec3 s = glm::sign(endPos_CS - startPos_CS);
    glm::ivec3 pos = startPos_CS;

    // Determine dominant axis
    const int axis0 = (d.x >= d.y && d.x >= d.z) ? 0 : (d.y >= d.z) ? 1 : 2;
    const int axis1 = (axis0 + 1) % 3;
    const int axis2 = (axis0 + 2) % 3;

    const int dm = d[axis0];
    int err1 = 2 * d[axis1] - dm;
    int err2 = 2 * d[axis2] - dm;
    const int dErr1 = 2 * d[axis1];
    const int dErr2 = 2 * d[axis2];
    const int dm2 = 2 * dm;

    for (int i = 0; i <= dm; ++i)
    {
        if (Chunk::isInChunk(pos))
        {
            tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(glm::uvec3(pos)), block);
        }

        if (err1 > 0)
        {
            pos[axis1] += s[axis1];
            err1 -= dm2;
        }
        if (err2 > 0)
        {
            pos[axis2] += s[axis2];
            err2 -= dm2;
        }
        err1 += dErr1;
        err2 += dErr2;
        pos[axis0] += s[axis0];
    }
}

inline std::vector<glm::vec3> buildSpline(const std::vector<glm::vec3>& ctrlPts, uint32_t numSplinePts)
{
    std::vector<glm::vec3> result;
    result.reserve(numSplinePts);

    const uint32_t n = static_cast<uint32_t>(ctrlPts.size());
    if (n == 0)
    {
        return result;
    }
    else if (n == 1)
    {
        result.push_back(ctrlPts[0]);
        return result;
    }

    std::vector<glm::vec3> pts;
    pts.resize(n);

    for (uint32_t i = 0; i < numSplinePts; ++i)
    {
        const float t = i / static_cast<float>(numSplinePts - 1);

        std::memcpy(pts.data(), ctrlPts.data(), n * sizeof(glm::vec3));
        for (uint32_t level = n - 1; level > 0; --level)
        {
            for (uint32_t j = 0; j < level; ++j)
            {
                pts[j] = glm::mix(pts[j], pts[j + 1], t);
            }
        }

        result.push_back(pts[0]);
    }

    return result;
}

inline void fillSpline(std::vector<Block>& blocks, const std::vector<glm::vec3>& spline, Block block)
{
    for (int i = 0; i < spline.size() - 1; ++i)
    {
        fillLine(blocks, glm::ivec3(glm::floor(spline[i])), glm::ivec3(glm::floor(spline[i + 1])), block);
    }
}

// With a nonzero droopChance, some bottom-layer columns (hashed by world XZ so the choice is
// chunk-independent) extend one block further down, imitating hanging moss
inline void placeLeafCap(std::vector<Block>& blocks,
                  glm::ivec3 centerPos_CS,
                  float minRadius,
                  float maxRadius,
                  float maxHeight,
                  RandomNumberGenerator& rng,
                  Block block,
                  float droopChance = 0.f,
                  glm::ivec2 chunkPosXZ_WS = {})
{
    const float radiusMultiplier = rng.nextFloat(0.9f, 1.1f);
    const int maxRadiusCeil = (int)glm::ceil(maxRadius * radiusMultiplier);
    const int yMax = centerPos_CS.y + (int)glm::floor(maxHeight - 0.5f);

    for (int y = centerPos_CS.y; y <= yMax; ++y)
    {
        const float posY = y - centerPos_CS.y + 0.5f;
        const float leavesRadius = glm::mix(maxRadius, minRadius, posY / maxHeight) * radiusMultiplier;
        const float leavesRadius2 = leavesRadius * leavesRadius;

        for (int dz = -maxRadiusCeil; dz <= maxRadiusCeil; ++dz)
        {
            for (int dx = -maxRadiusCeil; dx <= maxRadiusCeil; ++dx)
            {
                if (dx * dx + dz * dz >= leavesRadius2)
                {
                    continue;
                }

                int droopDepth = 0;
                if (droopChance > 0.f && y == centerPos_CS.y)
                {
                    const glm::ivec2 columnPosXZ_WS = chunkPosXZ_WS + glm::ivec2(centerPos_CS.x + dx, centerPos_CS.z + dz);
                    RandomNumberGenerator droopRng = initRng(SettingsManager::getWorldSeed() ^ hash(273904811),
                                                             static_cast<uint32_t>(columnPosXZ_WS.x),
                                                             static_cast<uint32_t>(columnPosXZ_WS.y /*z*/));
                    if (droopRng.chance(droopChance))
                    {
                        droopDepth = 1;
                    }
                }

                for (int dy = -droopDepth; dy <= 0; ++dy)
                {
                    const glm::ivec3 pos_CS(centerPos_CS.x + dx, y + dy, centerPos_CS.z + dz);
                    if (Chunk::isInChunk(pos_CS))
                    {
                        tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(glm::uvec3(pos_CS)), block, false /*canReplaceWater*/);
                    }
                }
            }
        }
    }
}

// Roughly spherical blob of leaves, slightly squashed in y, radius jittered ±10%
inline void placeLeafBlob(std::vector<Block>& blocks, glm::ivec3 centerPos_CS, float radius, RandomNumberGenerator& rng, Block block)
{
    const float r = radius * rng.nextFloat(0.9f, 1.1f);
    constexpr float ySquash = 0.75f;
    const float r2 = r * r;
    const int radiusCeilXZ = (int)glm::ceil(r);
    const int radiusCeilY = (int)glm::ceil(r * ySquash);

    for (int dy = -radiusCeilY; dy <= radiusCeilY; ++dy)
    {
        const float scaledDy = dy / ySquash;
        for (int dz = -radiusCeilXZ; dz <= radiusCeilXZ; ++dz)
        {
            for (int dx = -radiusCeilXZ; dx <= radiusCeilXZ; ++dx)
            {
                if (dx * dx + scaledDy * scaledDy + dz * dz >= r2)
                {
                    continue;
                }
                const glm::ivec3 pos_CS = centerPos_CS + glm::ivec3(dx, dy, dz);
                if (!Chunk::isInChunk(pos_CS))
                {
                    continue;
                }
                tryPlaceStructureBlock(blocks, Chunk::blockPosToIdx(glm::uvec3(pos_CS)), block, false /*canReplaceWater*/);
            }
        }
    }
}

} // namespace StructureHelpers
