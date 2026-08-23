// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "swamp_shaping.h"

#include "debug.h"
#include "rendering/common/common_settings.h"
#include "util/math.h"
#include "util/rng.h"

#include <limits>

using namespace glm;

namespace SwampShaping
{

inline constexpr int seaLevel = SEA_LEVEL;

inline constexpr int swampCellSize = 128;
inline constexpr int swampCellPadding = 32;
inline constexpr int swampLevelQuantize = 4;
inline constexpr float swampTerrainSurfaceMultiplier = 0.4f;
// Band above the pond level over which pulled-down marsh terrain blends back to natural
inline constexpr float swampPullDownStart = 14.f;
inline constexpr float swampPullDownBlendRange = 22.f;
// Edge distance within which a dam stays at full height; must exceed the warp jitter between
// adjacent columns so open water can never sit directly next to an unraised column
inline constexpr float swampDamCoreDist = 6.f;
// The far-cell gate must reach zero before the closest possible site distance at which adjacent
// columns' scan windows can disagree about a cell, or contributions seam along the window-flip
// contour; both bounds scale with swampCellSize
inline constexpr float swampFarCellGateStartDist = 90.f;
inline constexpr float swampFarCellGateEndDist = 130.f;
// Edge-distance ends of the containment band and surface-noise flatten ramps (both start at
// swampDamCoreDist); the containment band must also reach zero before the window shifts
inline constexpr float swampContainmentBandEndDist = 18.f;
inline constexpr float swampFlattenFadeEndDist = 60.f;

// 5x5, not 3x3: a 3x3 window can miss the true nearest site for one of two adjacent columns
inline constexpr int swampCellScanRadius = 2;
inline constexpr int swampCellScanWidth = 2 * swampCellScanRadius + 1;
inline constexpr int numScannedCells = swampCellScanWidth * swampCellScanWidth;

static uint32_t worldSeed;

void init(uint32_t seed)
{
    worldSeed = seed;
}

// Blends surface multipliers linearly in amplitude (1 / multiplier) space: the surface offset is
// noise / multiplier, so mixing multipliers directly compresses most of the amplitude change into
// the low-multiplier end of the ramp and produces steep slopes there.
static float mixSurfaceMultiplierByAmplitude(float multA, float multB, float t)
{
    return 1.f / glm::mix(1.f / multA, 1.f / multB, t);
}

// Swamp cells use a plain square grid, NOT ChunkGenerator's staggered structure grid — the
// odd-row x shift would make corner + offset * swampCellSize enumerate cells that don't exist.
static ivec2 swampCellCornerForPosXZ_WS(ivec2 posXZ_WS)
{
    return ivec2(MathUtil::floorDiv(posXZ_WS.x, swampCellSize) * swampCellSize,
                 MathUtil::floorDiv(posXZ_WS.y /*z*/, swampCellSize) * swampCellSize);
}

static ivec2 swampCellSiteXZ_WS(ivec2 cellCornerXZ_WS)
{
    RandomNumberGenerator rng = initRng(worldSeed ^ hash(529817231), cellCornerXZ_WS.x, cellCornerXZ_WS.y /*z*/);
    constexpr int innerSide = swampCellSize - 2 * swampCellPadding;
    return cellCornerXZ_WS + ivec2(swampCellPadding) + ivec2(rng.nextInt(innerSide), rng.nextInt(innerSide));
}

static CellInfo computeSwampCellInfo(ivec2 cellCornerXZ_WS)
{
    const ivec2 siteXZ_WS = swampCellSiteXZ_WS(cellCornerXZ_WS);
    const BiomeNoise siteNoise = BiomeNoiseFields::sampleAt(vec2(siteXZ_WS));

    // The pond level tracks the second-lowest of nine natural-height samples across the cell
    float minNaturalBase = BiomeNoiseFields::computeNaturalTerrain(siteNoise).baseHeight;
    float secondMinNaturalBase = std::numeric_limits<float>::max();
    for (int sampleIdx = 0; sampleIdx < 9; ++sampleIdx)
    {
        if (sampleIdx == 4)
        {
            continue; // cell center: the (jittered) site sample already covers it
        }

        const ivec2 sampleXZ_WS = cellCornerXZ_WS + (swampCellSize / 2) * ivec2(sampleIdx % 3, sampleIdx / 3);
        const float sampleBase =
            BiomeNoiseFields::computeNaturalTerrain(BiomeNoiseFields::sampleAt(vec2(sampleXZ_WS))).baseHeight;
        if (sampleBase < minNaturalBase)
        {
            secondMinNaturalBase = minNaturalBase;
            minNaturalBase = sampleBase;
        }
        else
        {
            secondMinNaturalBase = min(secondMinNaturalBase, sampleBase);
        }
    }

    const int naturalBase = static_cast<int>(std::floor(secondMinNaturalBase));
    return {
        .swampy = BiomeNoiseFields::computeFloodFactor(siteNoise) > BiomeNoiseFields::floodCellThreshold,
        .pondLevel = std::max((naturalBase - 2) / swampLevelQuantize * swampLevelQuantize, seaLevel + 3),
    };
}

ChunkContext makeChunkContext(ivec2 chunkPosBlocksXZ_WS, int chunkSizeBlocksXZ)
{
    // Conservative bound on how far any of the chunk's warped column positions can land
    constexpr int warpBound = static_cast<int>(swampWarpAmplitude + swampWarpFineAmplitude) + 1;
    const ivec2 minCenterCorner = swampCellCornerForPosXZ_WS(chunkPosBlocksXZ_WS - warpBound);
    const ivec2 maxCenterCorner =
        swampCellCornerForPosXZ_WS(chunkPosBlocksXZ_WS + chunkSizeBlocksXZ - 1 + warpBound);

    ChunkContext context;
    context.siteGridMinCornerXZ_WS = minCenterCorner - swampCellScanRadius * swampCellSize;
    const ivec2 maxCornerXZ_WS = maxCenterCorner + swampCellScanRadius * swampCellSize;
    context.siteGridNumCells = (maxCornerXZ_WS - context.siteGridMinCornerXZ_WS) / swampCellSize + 1;

    context.sites.reserve(static_cast<size_t>(context.siteGridNumCells.x) * context.siteGridNumCells.y);
    for (int cellZ = 0; cellZ < context.siteGridNumCells.y /*z*/; ++cellZ)
    {
        for (int cellX = 0; cellX < context.siteGridNumCells.x; ++cellX)
        {
            context.sites.push_back(
                swampCellSiteXZ_WS(context.siteGridMinCornerXZ_WS + ivec2(cellX, cellZ) * swampCellSize));
        }
    }
    return context;
}

Shaping computeShaping(vec2 warpedPosXZ_WS,
                       const BiomeNoise& biomeNoise,
                       const BiomeNoiseFields::NaturalTerrain& naturalTerrain,
                       ChunkContext& context)
{
    const auto siteAt = [&context](ivec2 cellCornerXZ_WS) -> ivec2
    {
        const ivec2 cellIdx = (cellCornerXZ_WS - context.siteGridMinCornerXZ_WS) / swampCellSize;
        ASSERT(cellIdx.x >= 0 && cellIdx.x < context.siteGridNumCells.x && cellIdx.y >= 0 &&
                   cellIdx.y < context.siteGridNumCells.y,
               "swamp cell outside the chunk's precomputed site grid");
        return context.sites[cellIdx.x + context.siteGridNumCells.x * cellIdx.y /*z*/];
    };

    const auto getSwampCellInfo = [&context](ivec2 cellCornerXZ_WS) -> CellInfo
    {
        for (const auto& [cachedCorner, cachedInfo] : context.cellCache)
        {
            if (cachedCorner == cellCornerXZ_WS)
            {
                return cachedInfo;
            }
        }
        return context.cellCache.emplace_back(cellCornerXZ_WS, computeSwampCellInfo(cellCornerXZ_WS)).second;
    };

    const ivec2 centerCellCornerXZ_WS = swampCellCornerForPosXZ_WS(ivec2(floor(warpedPosXZ_WS)));

    float dists[numScannedCells];
    ivec2 cellCorners[numScannedCells];
    int nearestIdx = 0;
    for (int offsetZ = -swampCellScanRadius; offsetZ <= swampCellScanRadius; ++offsetZ)
    {
        for (int offsetX = -swampCellScanRadius; offsetX <= swampCellScanRadius; ++offsetX)
        {
            const int idx = (offsetX + swampCellScanRadius) + swampCellScanWidth * (offsetZ + swampCellScanRadius);
            cellCorners[idx] = centerCellCornerXZ_WS + ivec2(offsetX, offsetZ) * swampCellSize;
            dists[idx] = length(vec2(siteAt(cellCorners[idx])) - warpedPosXZ_WS);
            if (dists[idx] < dists[nearestIdx])
            {
                nearestIdx = idx;
            }
        }
    }

    const CellInfo nearestCell = getSwampCellInfo(cellCorners[nearestIdx]);
    const float deepFloodMix =
        smoothstep(BiomeNoiseFields::floodCellThreshold, 0.9f, BiomeNoiseFields::computeFloodFactor(biomeNoise));

    // Shape height this column would take under the given cell: pond-floor pull-down for swampy
    // cells, untouched natural terrain otherwise. Natural terrain below the marsh floor is left
    // untouched and becomes deeper open water.
    const auto shapeHeightForCell = [&](const CellInfo& cell, float& outNaturalMix)
    {
        outNaturalMix = 1.f;
        if (!cell.swampy)
        {
            return naturalTerrain.baseHeight;
        }
        const float marshFloorHeight = static_cast<float>(cell.pondLevel) - glm::mix(2.f, 5.f, deepFloodMix);
        const float heightAboveLevel = naturalTerrain.baseHeight - static_cast<float>(cell.pondLevel);
        outNaturalMix = smoothstep(swampPullDownStart, swampPullDownStart + swampPullDownBlendRange, heightAboveLevel);
        if (naturalTerrain.baseHeight <= marshFloorHeight)
        {
            return naturalTerrain.baseHeight;
        }
        return glm::mix(marshFloorHeight, naturalTerrain.baseHeight, outNaturalMix);
    };

    Shaping result = {
        .baseHeight = naturalTerrain.baseHeight,
        .surfaceMultiplier = naturalTerrain.surfaceMultiplier,
        .waterLevel = seaLevel,
        .caveSeal = 0,
    };

    float nearestNaturalMix;
    const float nearestShapeHeight = shapeHeightForCell(nearestCell, nearestNaturalMix);
    if (nearestCell.swampy)
    {
        result.baseHeight = nearestShapeHeight;
        // The multiplier flattens only where the base was modified.
        result.surfaceMultiplier =
            mixSurfaceMultiplierByAmplitude(swampTerrainSurfaceMultiplier, naturalTerrain.surfaceMultiplier, nearestNaturalMix);
        result.waterLevel = nearestCell.pondLevel;
        result.caveSeal = nearestCell.pondLevel;
    }

    // Anchor for the dam contributions below, blended across near-tied cells' shape heights so it
    // stays continuous when the nearest-cell identity flips
    constexpr float anchorBlendDist = 12.f;
    float anchorBase = 0.f;
    float anchorWeightSum = 0.f;
    for (int idx = 0; idx < numScannedCells; ++idx)
    {
        const float tieDist = dists[idx] - dists[nearestIdx];
        if (tieDist >= anchorBlendDist)
        {
            continue;
        }

        float unusedNaturalMix;
        const float shapeHeight = (idx == nearestIdx)
            ? nearestShapeHeight
            : shapeHeightForCell(getSwampCellInfo(cellCorners[idx]), unusedNaturalMix);
        const float weight = 1.f - smoothstep(0.f, anchorBlendDist, tieDist);
        anchorBase += weight * shapeHeight;
        anchorWeightSum += weight;
    }
    anchorBase /= anchorWeightSum; // the nearest cell always contributes weight 1

    float flattenMixMax = 0.f;
    for (int idx = 0; idx < numScannedCells; ++idx)
    {
        if (idx == nearestIdx)
        {
            continue;
        }

        const float edgeDist = dists[idx] - dists[nearestIdx];

        // The containment band overrides the gate: the dam is the only water containment
        const float farCellGate = 1.f - smoothstep(swampFarCellGateStartDist, swampFarCellGateEndDist, dists[idx]);
        const float containmentBand = 1.f - smoothstep(swampDamCoreDist, swampContainmentBandEndDist, edgeDist);
        const float cellGate = max(farCellGate, containmentBand);
        // A fully gated-out cell contributes exactly nothing, so skip it before paying for its info
        if (cellGate <= 0.f)
        {
            continue;
        }

        const CellInfo neighborCell = getSwampCellInfo(cellCorners[idx]);
        if (nearestCell.swampy)
        {
            if (neighborCell.swampy && neighborCell.pondLevel == nearestCell.pondLevel)
            {
                continue; // same level: ponds merge, no dam between them
            }
        }
        else if (!neighborCell.swampy)
        {
            continue;
        }

        // Seal caves below any pond close enough for its water to reach this column.
        if (neighborCell.swampy)
        {
            result.caveSeal =
                std::max(result.caveSeal, static_cast<int>(static_cast<float>(neighborCell.pondLevel) * cellGate));
        }

        // The flatten ramps on edge distance, not dam rise
        flattenMixMax = max(flattenMixMax, (1.f - smoothstep(swampDamCoreDist, swampFlattenFadeEndDist, edgeDist)) * cellGate);

        int maxPondLevel = neighborCell.swampy ? neighborCell.pondLevel : seaLevel;
        if (nearestCell.swampy)
        {
            maxPondLevel = std::max(maxPondLevel, nearestCell.pondLevel);
        }
        // The backside tail slopes the levee backside down instead of cliffing; the gate scales
        // the whole profile toward the anchor
        const float artificialRise = static_cast<float>(maxPondLevel + 2) - naturalTerrain.baseHeight;
        const float backsideTail =
            1.f - smoothstep(swampDamCoreDist, swampDamCoreDist + 4.f * max(artificialRise, 1.f), edgeDist);
        const float damHeight =
            glm::mix(anchorBase, naturalTerrain.baseHeight + max(0.f, artificialRise) * backsideTail, cellGate);
        const float damRise = damHeight - anchorBase;
        if (damRise <= 0.f)
        {
            continue;
        }

        // Constant-slope bank: a fixed run of blocks per block of rise
        const float fadeEndDist = swampDamCoreDist + min(10.f * damRise, 64.f);
        const float damMix = 1.f - smoothstep(swampDamCoreDist, fadeEndDist, edgeDist);
        if (damMix <= 0.f)
        {
            continue;
        }

        const float baseTowardDam = glm::mix(anchorBase, damHeight, damMix);
        result.baseHeight = max(result.baseHeight, baseTowardDam);
    }

    // Applied to swampy columns too: their naturalMix-based multiplier must still reach the
    // flattened value at borders so it meets the neighbor side continuously.
    if (flattenMixMax > 0.f)
    {
        result.surfaceMultiplier =
            mixSurfaceMultiplierByAmplitude(result.surfaceMultiplier, swampTerrainSurfaceMultiplier, flattenMixMax);
    }

    return result;
}

} // namespace SwampShaping
