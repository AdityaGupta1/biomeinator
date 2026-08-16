// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "chunk_generator.h"

#include "biome.h"
#include "biome_noise.h"
#include "cave_biome.h"
#include "chunk.h"
#include "rendering/common/common_settings.h"
#include "settings_manager.h"
#include "multithreading/thread_memory_allocator.h"
#include "util/glm_util.h"
#include "util/rng.h"

#include <set>
#include <vector>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace ChunkGenerator
{

static FN::SmartNode<FN::Generator> fnTerrainBase;

inline constexpr float caveWorleyBoundFraction = 0.4f;
inline constexpr float caveSimplexBoundFraction = 0.6f;
// caves are fully suppressed by altitude squash well before this height
inline constexpr int caveAbsoluteMaxY = 320;

static FN::SmartNode<FN::Generator> fnCavesWorley;
static FN::SmartNode<FN::Generator> fnCavesSimplex;

// Cave biome temperature/humidity are sampled on a coarse grid (one sample per
// caveBiomeNoiseDownsample blocks per axis) and trilinearly interpolated. Biome
// regions are far larger than a block, so this costs ~1/64 of a full-resolution
// 3D field with no visible difference. caveBiomeSurfaceNoiseBias mixes in the column's
// 2D temperature/humidity so cave biomes loosely track the surface above them.
inline constexpr int caveBiomeNoiseDownsample = 4;
inline constexpr float caveBiomeSurfaceNoiseBias = 0.3f;

static FN::SmartNode<FN::Generator> fnCaveTemperature;
static FN::SmartNode<FN::Generator> fnCaveHumidity;

static FN::SmartNode<FN::Generator> fnSwampWarp;
static FN::SmartNode<FN::Generator> fnSwampWarpFine;

static uint worldSeed;
static ivec2 noiseOffsetXZ;

void init()
{
    worldSeed = SettingsManager::getWorldSeed();
    BiomeNoiseField::init(worldSeed);
    noiseOffsetXZ = BiomeNoiseField::getNoiseOffsetXZ();

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(210393129);
        fnSimplex->SetScale(400.0f);
        fnSimplex->SetOutputMin(-0.5f);
        fnSimplex->SetOutputMax(0.5f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(5);

        fnTerrainBase = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(918273645);
        fnSimplex->SetScale(90.0f);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);

        fnSwampWarp = fnSimplex;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(131071193);
        fnSimplex->SetScale(32.0f);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);

        fnSwampWarpFine = fnSimplex;
    }

    {
        auto fnCellular = FN::New<FN::CellularDistance>();
        fnCellular->SetSeedOffset(86839821);
        fnCellular->SetDistanceIndex0(2);
        fnCellular->SetDistanceIndex1(0);
        fnCellular->SetReturnType(FN::CellularDistance::ReturnType::Index0Div1);
        fnCellular->SetScale(80.f);
        auto fnDomainWarp = FN::New<FN::DomainWarpGradient>();
        fnDomainWarp->SetSource(fnCellular);
        fnDomainWarp->SetSeedOffset(302341102);
        fnDomainWarp->SetWarpAmplitude(50.f);
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetScale(800.f);
        fnSimplex->SetOutputMin(-0.3f);
        fnSimplex->SetOutputMax(0.3f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnDomainWarp);
        fnAdd->SetRHS(fnSimplex);

        fnCavesWorley = fnAdd;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetScale(100.f);
        fnSimplex->SetOutputMin(0.0f);
        fnSimplex->SetOutputMax(0.8f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(3);
        auto fnDomainWarp = FN::New<FN::DomainWarpGradient>();
        fnDomainWarp->SetSource(fnFractal);
        fnDomainWarp->SetSeedOffset(509920112);
        fnDomainWarp->SetWarpAmplitude(20.f);

        fnCavesSimplex = fnDomainWarp;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(418203741);
        fnSimplex->SetScale(800.f);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);

        fnCaveTemperature = fnSimplex;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(992013047);
        fnSimplex->SetScale(800.f);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);

        fnCaveHumidity = fnSimplex;
    }
}

static inline void fillNoiseArray3D(float* data, const FN::SmartNode<FN::Generator>& fn, glm::ivec2 posXZ, uint height, int yOffset = 0)
{
    fn->GenUniformGrid3D(data,
                         yOffset /*y*/,
                         posXZ.x + noiseOffsetXZ.x /*x*/,
                         posXZ.y + noiseOffsetXZ.y /*z*/,
                         height,
                         chunkSizeXZ,
                         chunkSizeXZ,
                         1.f,
                         1.f,
                         1.f,
                         worldSeed ^ hash(391023545));
}

// Fills a coarse grid stepping caveBiomeNoiseDownsample blocks per axis, starting at world y=0.
// The coarse origin is the chunk origin (a multiple of chunkSizeXZ, hence of the downsample), so
// adjacent chunks sample identical world positions on their shared border and biomes stay seamless.
static inline void fillCaveBiomeNoiseArray(float* data, const FN::SmartNode<FN::Generator>& fn, glm::ivec2 posXZ, uint coarseSizeXZ, uint coarseHeight)
{
    fn->GenUniformGrid3D(data,
                         0.f /*y*/,
                         posXZ.x + noiseOffsetXZ.x /*x*/,
                         posXZ.y + noiseOffsetXZ.y /*z*/,
                         coarseHeight,
                         coarseSizeXZ,
                         coarseSizeXZ,
                         caveBiomeNoiseDownsample,
                         caveBiomeNoiseDownsample,
                         caveBiomeNoiseDownsample,
                         worldSeed ^ hash(391023545));
}

// Trilinearly samples a coarse cave biome field at a chunk-local block position. The coarse layout
// mirrors fillNoiseArray3D: world y is contiguous (grid x-axis), then world x, then world z.
static inline float sampleCaveBiomeNoise(const float* coarseNoise, uint coarseSizeXZ, uint coarseHeight, uint blockX, uint y, uint blockZ)
{
    constexpr float invDownsample = 1.f / caveBiomeNoiseDownsample;
    const float gridX = blockX * invDownsample;
    const float gridY = y * invDownsample;
    const float gridZ = blockZ * invDownsample;

    const uint x0 = static_cast<uint>(gridX);
    const uint y0 = static_cast<uint>(gridY);
    const uint z0 = static_cast<uint>(gridZ);
    const float tx = gridX - x0;
    const float ty = gridY - y0;
    const float tz = gridZ - z0;

    const auto sample = [&](uint cellX, uint cellY, uint cellZ) -> float {
        const uint idx = (cellZ * coarseSizeXZ + cellX) * coarseHeight + cellY;
        return coarseNoise[idx];
    };

    const float c00 = glm::mix(sample(x0, y0, z0), sample(x0 + 1, y0, z0), tx);
    const float c10 = glm::mix(sample(x0, y0 + 1, z0), sample(x0 + 1, y0 + 1, z0), tx);
    const float c01 = glm::mix(sample(x0, y0, z0 + 1), sample(x0 + 1, y0, z0 + 1), tx);
    const float c11 = glm::mix(sample(x0, y0 + 1, z0 + 1), sample(x0 + 1, y0 + 1, z0 + 1), tx);

    const float c0 = glm::mix(c00, c10, ty);
    const float c1 = glm::mix(c01, c11, ty);

    return glm::mix(c0, c1, tz);
}

// Finds the grid cell containing posXZ_WS (accounting for the staggered odd-row x shift) and
// returns its corner. Shared by surface and cave placement so both lay grids over the same cells.
static inline ivec2 gridCellCornerForPosXZ_WS(ivec2 posXZ_WS, int gridCellSideLength)
{
    const int halfCell = gridCellSideLength / 2;
    const int gridZ = MathUtil::floorDiv(posXZ_WS.y /*z*/, gridCellSideLength);
    const int rowShiftX = (gridZ & 1) ? halfCell : 0; // odd rows shift half a cell in x (staggered/brick layout)
    const int gridX = MathUtil::floorDiv(posXZ_WS.x - rowShiftX, gridCellSideLength);
    return ivec2(gridX * gridCellSideLength + rowShiftX, gridZ * gridCellSideLength);
}

// Maps a grid cell corner to its single deterministic candidate XZ, jittered within an inner
// region inset by padding on the cell's high edge (innerSide = gridCellSideLength - padding).
// rngSeed/rngArg4 let surface and cave seed independent grids over the same cells.
static inline ivec2 gridCellCandidateXZ_WS(ivec2 cellCornerXZ_WS, int innerSide, uint rngSeed, uint rngArg4)
{
    RandomNumberGenerator rng = initRng(rngSeed, cellCornerXZ_WS.x, cellCornerXZ_WS.y /*z*/, rngArg4);
    return cellCornerXZ_WS + ivec2(rng.nextInt(innerSide), rng.nextInt(innerSide));
}

}; // namespace ChunkGenerator

using namespace ChunkGenerator;

inline constexpr float terrainBelowHeightfieldSurfaceMultiplier = 2.f;
inline constexpr float surfaceValBound = 1.2f; // noise is approximately between -1 and 1, so +/- 1.2 means we can be absolutely sure that this is terrain or air

inline constexpr int seaLevel = SEA_LEVEL;

// Swamps are cellular ponds: a sparse jittered grid of cell sites, where each swampy cell floods
// to its own flat water level derived from the local natural terrain height. Terrain inside a
// swampy cell is sunk just below the pond level, and a raised dam band along every cell border
// contains the water — two different water levels never touch, and the dam is the only containment
// mechanism, so no per-column gate may suppress it. Cells whose neighbors share a level merge into
// larger marshes by skipping the dam between them.
inline constexpr int swampCellSize = 128;
inline constexpr int swampCellPadding = 32;
inline constexpr int swampLevelQuantize = 4;
inline constexpr float swampTerrainSurfaceMultiplier = 0.4f;
// Swamp shaping is height-domain: natural terrain up to the pull-down start above the pond level
// is pulled down to the marsh flat, then blends back to fully natural over the blend range. The
// transition width therefore scales with the natural slope, and tall terrain is never fought —
// hills form the shore instead of a dam wall. The band is deliberately NOT widened by the flood
// factor: a spatially varying band start turns flood-factor contours on hillsides into sunken
// dry shelves (trenches); deep flood expresses itself through the marsh floor depth instead.
inline constexpr float swampPullDownStart = 14.f;
inline constexpr float swampPullDownBlendRange = 16.f;
// Column positions are domain-warped before the cell lookup so pond shorelines and dam bands
// wobble organically instead of following straight Voronoi edges. The amplitude must stay well
// under swampCellPadding so a warped column still finds its true nearest sites in the 3x3 scan.
inline constexpr float swampWarpAmplitude = 18.f;
inline constexpr float swampWarpFineAmplitude = 6.f;

struct NaturalTerrain
{
    float baseHeight;
    float surfaceMultiplier;
};

struct SwampCellInfo
{
    ivec2 siteXZ_WS;
    bool swampy;
    int pondLevel;
};

// Blends surface multipliers linearly in amplitude (1 / multiplier) space: the surface offset is
// noise / multiplier, so mixing multipliers directly compresses most of the amplitude change into
// the low-multiplier end of the ramp and produces steep slopes there.
static float mixSurfaceMultiplierByAmplitude(float multA, float multB, float t)
{
    return 1.f / glm::mix(1.f / multA, 1.f / multB, t);
}

static NaturalTerrain computeNaturalTerrain(const BiomeNoise& biomeNoise)
{
    const float scaledPeak = (biomeNoise.peak + 1.f) * 0.5f;

    float baseHeight = 140.f;
    baseHeight += pow(scaledPeak * max(biomeNoise.inland, 0.1f), 4.f) * 135.f;
    const float inlandHeightModifier = 1.f / (1.f + expf(-10.f * biomeNoise.inland + 0.1f)) + 0.03f * biomeNoise.inland - 0.7f;
    baseHeight += inlandHeightModifier * 90.f;
    const float seaLevelPullFactor = smoothstep(0.2f, 0.0f, abs(biomeNoise.inland)) * 0.9f;
    baseHeight = glm::mix(baseHeight, static_cast<float>(seaLevel + 8), seaLevelPullFactor);

    float surfaceMultiplier = 0.02f;
    surfaceMultiplier -= scaledPeak * 0.008f;
    surfaceMultiplier *= 3.f * smoothstep(0.4f, -0.1f, abs(biomeNoise.inland)) + 1.f;

    return { baseHeight, surfaceMultiplier };
}

// Swamp cells use a plain square grid, NOT gridCellCornerForPosXZ_WS — that helper's staggered
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

static SwampCellInfo computeSwampCellInfo(ivec2 cellCornerXZ_WS)
{
    const ivec2 siteXZ_WS = swampCellSiteXZ_WS(cellCornerXZ_WS);
    const BiomeNoise siteNoise = BiomeNoiseField::sampleAt(vec2(siteXZ_WS));

    // The pond level tracks the low end of the natural height across the cell (site, corners, and
    // edge midpoints), so a cell on sloped terrain floods its low side instead of perching a deep
    // pond above it. The second-lowest sample is used so a single deep corner can't drag the level
    // below the rest of the cell and leave it dry. Boundary samples are shared with adjacent cells,
    // which also nudges neighboring levels toward merging.
    float minNaturalBase = computeNaturalTerrain(siteNoise).baseHeight;
    float secondMinNaturalBase = std::numeric_limits<float>::max();
    for (int sampleIdx = 0; sampleIdx < 9; ++sampleIdx)
    {
        if (sampleIdx == 4)
        {
            continue; // cell center: the (jittered) site sample already covers it
        }

        const ivec2 sampleXZ_WS = cellCornerXZ_WS + (swampCellSize / 2) * ivec2(sampleIdx % 3, sampleIdx / 3);
        const float sampleBase = computeNaturalTerrain(BiomeNoiseField::sampleAt(vec2(sampleXZ_WS))).baseHeight;
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
        .siteXZ_WS = siteXZ_WS,
        .swampy = BiomeNoiseField::computeFloodFactor(siteNoise) > BiomeNoiseField::floodCellThreshold,
        .pondLevel = std::max((naturalBase - 2) / swampLevelQuantize * swampLevelQuantize, seaLevel + 3),
    };
}

// y of the lava surface (the low-y lava fill writes LAVA_TOP at y == 4); cave structures
// whose anchor sits at or below this are rejected unless flagged to allow lava.
inline constexpr int lavaSurfaceY = 4;

void Chunk::fillTerrainBlocksAndCreateStructures(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const ivec2 chunkPosBlocksXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    float* temperatureNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* humidityNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* peakNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* inlandNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    const BiomeNoiseField::BiomeNoiseGrids biomeNoiseGrids = {
        .temperature = temperatureNoise,
        .humidity = humidityNoise,
        .peak = peakNoise,
        .inland = inlandNoise,
    };
    BiomeNoiseField::fillGrids(biomeNoiseGrids, vec2(chunkPosBlocksXZ_WS), uvec2(chunkSizeXZ), 1.f);

    float* terrainBaseHeightArray = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* terrainSurfaceMultiplierArray = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    int* waterLevelArray = threadMemoryAlloc.request<int>(chunkSizeXZSquare);
    int* swampCaveSealArray = threadMemoryAlloc.request<int>(chunkSizeXZSquare);

    float* swampWarpXNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* swampWarpZNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* swampWarpFineXNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    float* swampWarpFineZNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare);
    const auto fillSwampWarpNoise = [&](float* data, const FN::SmartNode<FN::Generator>& fn, uint seedSalt)
    {
        fn->GenUniformGrid2D(data,
                             chunkPosBlocksXZ_WS.x + noiseOffsetXZ.x,
                             chunkPosBlocksXZ_WS.y + noiseOffsetXZ.y /*z*/,
                             chunkSizeXZ,
                             chunkSizeXZ,
                             1.f,
                             1.f,
                             static_cast<int>(worldSeed ^ hash(seedSalt)));
    };
    fillSwampWarpNoise(swampWarpXNoise, fnSwampWarp, 651209371);
    fillSwampWarpNoise(swampWarpZNoise, fnSwampWarp, 287119023);
    fillSwampWarpNoise(swampWarpFineXNoise, fnSwampWarpFine, 907812341);
    fillSwampWarpNoise(swampWarpFineZNoise, fnSwampWarpFine, 412093871);

    int terrainNoiseMinY = chunkSizeY;
    int terrainNoiseMaxY = 0;
    int waterLevelMax = seaLevel;
    float terrainBaseHeightMin = std::numeric_limits<float>::max();
    float terrainBaseHeightMax = std::numeric_limits<float>::lowest();

    std::set<Biome> biomeSet;

    RandomNumberGenerator rng = initRng(worldSeed ^ hash(330910521), chunkPosBlocksXZ_WS.x, chunkPosBlocksXZ_WS.y /*z*/);

    // A chunk touches only a handful of unique swamp cells, so cache their info (each computation
    // costs several single-point noise samples).
    std::vector<std::pair<ivec2, SwampCellInfo>> swampCellCache;
    const auto getSwampCellInfo = [&swampCellCache](ivec2 cellCornerXZ_WS) -> SwampCellInfo
    {
        for (const auto& [cachedCorner, cachedInfo] : swampCellCache)
        {
            if (cachedCorner == cellCornerXZ_WS)
            {
                return cachedInfo;
            }
        }
        return swampCellCache.emplace_back(cellCornerXZ_WS, computeSwampCellInfo(cellCornerXZ_WS)).second;
    };

    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(blockX, blockZ);
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const BiomeNoise biomeNoise = BiomeNoiseField::noiseAt(biomeNoiseGrids, columnIdx);
            const BiomeNoise jitteredBiomeNoise = BiomeNoise::randomOffset(biomeNoise, rng);
            const Biome biome = BiomeNoiseField::biomeFromNoise(jitteredBiomeNoise);
            this->biomes[columnIdx] = biome;
            biomeSet.insert(biome);

            const NaturalTerrain naturalTerrain = computeNaturalTerrain(biomeNoise);
            float terrainBaseHeight = naturalTerrain.baseHeight;
            float terrainSurfaceMultiplier = naturalTerrain.surfaceMultiplier;
            int waterLevel = seaLevel;

            // The nearest cell site decides this column's pond; every other site contributes a dam
            // barrier profile. Taking the max over all sites (instead of just the second-nearest)
            // keeps the terrain continuous where the second-nearest site's identity flips, e.g. at
            // three-cell corners between merged and dry cells.
            {
                const vec2 warpedPosXZ_WS = vec2(blockPosXZ_WS) +
                    swampWarpAmplitude * vec2(swampWarpXNoise[columnIdx], swampWarpZNoise[columnIdx]) +
                    swampWarpFineAmplitude * vec2(swampWarpFineXNoise[columnIdx], swampWarpFineZNoise[columnIdx]);
                const ivec2 centerCellCornerXZ_WS = swampCellCornerForPosXZ_WS(ivec2(floor(warpedPosXZ_WS)));

                // 5x5, not 3x3: adjacent columns' windows are centered on their warped cells, which
                // can differ near grid lines — a 3x3 window can miss the true nearest site for one
                // of the two, making neighboring columns disagree about which pond they're in.
                constexpr int swampCellScanWidth = 5;
                constexpr int numScannedCells = swampCellScanWidth * swampCellScanWidth;
                float dists[numScannedCells];
                ivec2 cellCorners[numScannedCells];
                int nearestIdx = 0;
                for (int offsetZ = -2; offsetZ <= 2; ++offsetZ)
                {
                    for (int offsetX = -2; offsetX <= 2; ++offsetX)
                    {
                        const int idx = (offsetX + 2) + swampCellScanWidth * (offsetZ + 2);
                        cellCorners[idx] = centerCellCornerXZ_WS + ivec2(offsetX, offsetZ) * swampCellSize;
                        dists[idx] = length(vec2(swampCellSiteXZ_WS(cellCorners[idx])) - warpedPosXZ_WS);
                        if (dists[idx] < dists[nearestIdx])
                        {
                            nearestIdx = idx;
                        }
                    }
                }

                const SwampCellInfo cell1 = getSwampCellInfo(cellCorners[nearestIdx]);
                const float deepFloodMix = smoothstep(BiomeNoiseField::floodCellThreshold, 0.9f, BiomeNoiseField::computeFloodFactor(biomeNoise));

                // Shape height this column would take under the given cell: pond-floor pull-down
                // for swampy cells, untouched natural terrain otherwise. Natural terrain below the
                // marsh floor is left untouched and becomes deeper open water.
                const auto shapeHeightForCell = [&](const SwampCellInfo& cell, float& outNaturalMix)
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

                float naturalMix1;
                const float shapeHeight1 = shapeHeightForCell(cell1, naturalMix1);
                if (cell1.swampy)
                {
                    terrainBaseHeight = shapeHeight1;
                    // The multiplier flattens only where the base was modified.
                    terrainSurfaceMultiplier = mixSurfaceMultiplierByAmplitude(
                        swampTerrainSurfaceMultiplier, naturalTerrain.surfaceMultiplier, naturalMix1);
                    waterLevel = cell1.pondLevel;
                }

                // Dam contributions below interpolate from an anchor blended across near-tied
                // cells' shape heights, so the anchor stays continuous when the nearest-cell
                // identity flips at a swampy/dry border. Anchoring at the own cell's shape height
                // alone jumps there (marsh floor vs natural), cutting walls wherever a farther
                // cell's partial dam wins the max on only one side of the border. Away from
                // borders the weights collapse to the own cell and the anchor equals its shape.
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
                        ? shapeHeight1
                        : shapeHeightForCell(getSwampCellInfo(cellCorners[idx]), unusedNaturalMix);
                    const float weight = 1.f - smoothstep(0.f, anchorBlendDist, tieDist);
                    anchorBase += weight * shapeHeight;
                    anchorWeightSum += weight;
                }
                anchorBase /= anchorWeightSum; // the nearest cell always contributes weight 1

                float flattenMixMax = 0.f;
                int swampCaveSeal = cell1.swampy ? cell1.pondLevel : 0;
                for (int idx = 0; idx < numScannedCells; ++idx)
                {
                    if (idx == nearestIdx)
                    {
                        continue;
                    }

                    const SwampCellInfo neighborCell = getSwampCellInfo(cellCorners[idx]);
                    if (neighborCell.swampy)
                    {
                        swampCaveSeal = std::max(swampCaveSeal, neighborCell.pondLevel);
                    }
                    if (cell1.swampy)
                    {
                        if (neighborCell.swampy && neighborCell.pondLevel == cell1.pondLevel)
                        {
                            continue; // same level: ponds merge, no dam between them
                        }
                    }
                    else if (!neighborCell.swampy)
                    {
                        continue;
                    }

                    const float edgeDist = dists[idx] - dists[nearestIdx];

                    // The multiplier flatten must ramp on edge distance alone — gating it on the dam
                    // rise (like the base raise below) would flip the noise amplitude discontinuously
                    // along the contour where the natural height crosses the dam height, cutting
                    // vertical cliff faces there.
                    flattenMixMax = max(flattenMixMax, 1.f - smoothstep(6.f, 60.f, edgeDist));

                    int maxPondLevel = neighborCell.swampy ? neighborCell.pondLevel : seaLevel;
                    if (cell1.swampy)
                    {
                        maxPondLevel = std::max(maxPondLevel, cell1.pondLevel);
                    }
                    // The artificial part of the dam (rise above natural terrain) decays past the
                    // full-dam band: edgeDist dips along every Voronoi edge, not just this cell's
                    // boundary, so without the decay a high pond paints ridges across unrelated
                    // low terrain tens of blocks away.
                    const float artificialRise = static_cast<float>(maxPondLevel + 2) - naturalTerrain.baseHeight;
                    const float allowedArtificialRise = max(0.f, artificialRise - max(edgeDist - 6.f, 0.f));
                    const float damHeight = naturalTerrain.baseHeight + allowedArtificialRise;
                    const float damRise = damHeight - anchorBase;
                    if (damRise <= 0.f)
                    {
                        continue;
                    }

                    // Constant-slope bank: full dam within edge 6 (so warp jitter between adjacent
                    // columns can never step from open water straight to an unraised column), then
                    // a fixed run of blocks per block of rise, so tall banks fade over a
                    // proportionally longer distance instead of becoming steeper.
                    const float fadeEndDist = 6.f + min(10.f * damRise, 64.f);
                    const float damMix = 1.f - smoothstep(6.f, fadeEndDist, edgeDist);
                    if (damMix <= 0.f)
                    {
                        continue;
                    }

                    const float baseTowardDam = glm::mix(anchorBase, damHeight, damMix);
                    terrainBaseHeight = max(terrainBaseHeight, baseTowardDam);
                }

                // Applied to swampy columns too: their naturalMix-based multiplier must still reach
                // the flattened value at borders so it meets the neighbor side continuously.
                if (flattenMixMax > 0.f)
                {
                    terrainSurfaceMultiplier = mixSurfaceMultiplierByAmplitude(
                        terrainSurfaceMultiplier, swampTerrainSurfaceMultiplier, flattenMixMax);
                }

                swampCaveSealArray[columnIdx] = swampCaveSeal;
            }

            waterLevelArray[columnIdx] = waterLevel;
            waterLevelMax = std::max(waterLevelMax, waterLevel);

            terrainBaseHeightArray[columnIdx] = terrainBaseHeight;
            terrainSurfaceMultiplierArray[columnIdx] = terrainSurfaceMultiplier;
            terrainBaseHeightMin = std::min(terrainBaseHeightMin, terrainBaseHeight);
            terrainBaseHeightMax = std::max(terrainBaseHeightMax, terrainBaseHeight);

            const int thisColumnTerrainMinY = static_cast<int>(std::floor(terrainBaseHeight - (surfaceValBound / (terrainSurfaceMultiplier * terrainBelowHeightfieldSurfaceMultiplier))));
            const int thisColumnTerrainMaxY = static_cast<int>(std::ceil(terrainBaseHeight + (surfaceValBound / terrainSurfaceMultiplier)));
            terrainNoiseMinY = std::min(terrainNoiseMinY, thisColumnTerrainMinY);
            terrainNoiseMaxY = std::max(terrainNoiseMaxY, thisColumnTerrainMaxY);
        }
    }

    terrainNoiseMinY = std::max(terrainNoiseMinY, 0);
    terrainNoiseMaxY = std::min(terrainNoiseMaxY, static_cast<int>(chunkSizeY));
    ASSERT(terrainNoiseMinY < terrainNoiseMaxY, "terrain noise range is empty or inverted");

    const uint terrainNoiseHeight = terrainNoiseMaxY - terrainNoiseMinY;

    // worley is read for y < caveSimplexBound, so max needed y across chunk is terrainBaseHeightMax * caveSimplexBoundFraction
    // simplex is read for y >= caveWorleyBound, so min needed y across chunk is terrainBaseHeightMin * caveWorleyBoundFraction
    const int caveNoiseMaxY = std::min(terrainNoiseMaxY, caveAbsoluteMaxY);
    const uint caveWorleyNoiseHeight = static_cast<uint>(std::min(caveNoiseMaxY, static_cast<int>(std::ceil(terrainBaseHeightMax * caveSimplexBoundFraction)) + 2));
    ASSERT(caveWorleyNoiseHeight > 0 && caveWorleyNoiseHeight <= static_cast<uint>(caveNoiseMaxY), "cave worley noise height out of range");
    const int caveSimplexNoiseMinY = std::max(0, static_cast<int>(std::floor(terrainBaseHeightMin * caveWorleyBoundFraction)) - 2);
    const uint caveSimplexNoiseHeight = static_cast<uint>(caveNoiseMaxY - caveSimplexNoiseMinY);
    ASSERT(caveSimplexNoiseHeight > 0 && caveSimplexNoiseHeight <= static_cast<uint>(caveNoiseMaxY), "cave simplex noise height out of range");

    float* terrainNoise = threadMemoryAlloc.request<float>(chunkSizeXZSquare * terrainNoiseHeight);
    float* caveNoiseWorley = threadMemoryAlloc.request<float>(chunkSizeXZSquare * caveWorleyNoiseHeight);
    float* caveNoiseSimplex = threadMemoryAlloc.request<float>(chunkSizeXZSquare * caveSimplexNoiseHeight);
    fillNoiseArray3D(terrainNoise, fnTerrainBase, chunkPosBlocksXZ_WS, terrainNoiseHeight, terrainNoiseMinY);
    fillNoiseArray3D(caveNoiseWorley, fnCavesWorley, chunkPosBlocksXZ_WS, caveWorleyNoiseHeight);
    fillNoiseArray3D(caveNoiseSimplex, fnCavesSimplex, chunkPosBlocksXZ_WS, caveSimplexNoiseHeight, caveSimplexNoiseMinY);

    // +1 cell on each XZ axis is the far-edge interpolation margin; +2 in y leaves room for the
    // top of the band to interpolate against the next coarse cell.
    const uint caveBiomeNoiseSizeXZ = chunkSizeXZ / caveBiomeNoiseDownsample + 1;
    const uint caveBiomeNoiseHeight = caveNoiseMaxY / caveBiomeNoiseDownsample + 2;
    const uint caveBiomeNoiseSize = caveBiomeNoiseSizeXZ * caveBiomeNoiseSizeXZ * caveBiomeNoiseHeight;
    float* caveTemperatureNoise = threadMemoryAlloc.request<float>(caveBiomeNoiseSize);
    float* caveHumidityNoise = threadMemoryAlloc.request<float>(caveBiomeNoiseSize);
    fillCaveBiomeNoiseArray(caveTemperatureNoise, fnCaveTemperature, chunkPosBlocksXZ_WS, caveBiomeNoiseSizeXZ, caveBiomeNoiseHeight);
    fillCaveBiomeNoiseArray(caveHumidityNoise, fnCaveHumidity, chunkPosBlocksXZ_WS, caveBiomeNoiseSizeXZ, caveBiomeNoiseHeight);

    uint* heightfield = threadMemoryAlloc.request<uint>(chunkSizeXZSquare);

    const uint terrainNoiseSize = chunkSizeXZSquare * terrainNoiseHeight;
    const uint caveWorleyNoiseSize = chunkSizeXZSquare * caveWorleyNoiseHeight;
    const uint caveSimplexNoiseSize = chunkSizeXZSquare * caveSimplexNoiseHeight;
    const uint maxFillY = min(static_cast<int>(chunkSizeY - 1), max(terrainNoiseMaxY, waterLevelMax));

    // Reused scratch: holds one column's air pockets at a time, cleared and refilled per column.
    std::vector<CaveLayer> columnLayers;

    // Decides cave structures for a single column the instant its y-scan finishes. Column-centric
    // (unlike the cell-centric surface pass) because layer data is per-column and ready immediately.
    const auto placeCaveStructuresForColumn = [&](ivec2 columnPosXZ_WS)
    {
        for (uint layerIdx = 0; layerIdx < columnLayers.size(); ++layerIdx)
        {
            const CaveLayer& layer = columnLayers[layerIdx];
            const int layerHeight = layer.end - layer.start;

            const auto tryPlaceSide = [&](CaveBiome biome, bool ceiling, int anchorY)
            {
                const std::vector<CaveStructureGen>& gens = CaveBiomes::getCaveBiomeData(biome).caveStructureGens;
                for (const CaveStructureGen& gen : gens)
                {
                    if (gen.generatesFromCeiling != ceiling)
                    {
                        continue;
                    }
                    if (layerHeight < static_cast<int>(gen.minLayerHeight))
                    {
                        continue;
                    }
                    if (anchorY <= lavaSurfaceY && !bool(gen.flags & CAVE_STRUCTURE_GEN_FLAG_ALLOW_LAVA))
                    {
                        continue;
                    }

                    const int gridCellSideLength = static_cast<int>(gen.gridCellSideLength);
                    const int innerSide = gridCellSideLength - static_cast<int>(gen.gridCellPadding);
                    const ivec2 cellCornerXZ_WS = gridCellCornerForPosXZ_WS(columnPosXZ_WS, gridCellSideLength);
                    // fold layerIdx into the seed so a feature-pos column doesn't stamp every one of its stacked layers
                    const uint rngSeed = worldSeed ^ hash(53198477u) ^ hash(static_cast<uint>(gen.type)) ^
                                         hash(static_cast<uint>(layerIdx) * 0x9e3779b9u);
                    const ivec2 candidateXZ_WS =
                        gridCellCandidateXZ_WS(cellCornerXZ_WS, innerSide, rngSeed, static_cast<uint>(gen.type));
                    if (candidateXZ_WS != columnPosXZ_WS)
                    {
                        continue;
                    }

                    this->caveStructures.emplace_back(
                        gen.type, ivec3(columnPosXZ_WS.x, anchorY, columnPosXZ_WS.y /*z*/), layerHeight);
                    return; // first passing gen wins for this side (gen-list order = priority)
                }
            };

            tryPlaceSide(layer.bottomBiome, false /*ceiling*/, layer.start + 1);
            if (layer.closed)
            {
                tryPlaceSide(layer.topBiome, true /*ceiling*/, layer.end);
            }
        }
    };

    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const ivec2 blockPosXZ_WS = chunkPosBlocksXZ_WS + ivec2(blockX, blockZ);
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const Biome biome = this->biomes[columnIdx];
            const BiomeData& biomeData = Biomes::getBiomeData(biome);
            const TopBlocks& topBlocks = biomeData.topBlocks;

            const uint baseBlockIdx = chunkSizeY * columnIdx;
            const int baseTerrainNoiseIdx = static_cast<int>(terrainNoiseHeight * columnIdx) - terrainNoiseMinY;
            const uint baseCaveWorleyNoiseIdx = caveWorleyNoiseHeight * columnIdx;
            const uint baseCaveSimplexNoiseIdx = caveSimplexNoiseHeight * columnIdx;

            blocks[baseBlockIdx + 0] = Block::BEDROCK;

            const float terrainBaseHeight = terrainBaseHeightArray[columnIdx];
            const float terrainSurfaceMultiplier = terrainSurfaceMultiplierArray[columnIdx];
            const int waterLevel = waterLevelArray[columnIdx];

            const float caveWorleyBound = terrainBaseHeight * caveWorleyBoundFraction;
            const float caveSimplexBound = terrainBaseHeight * caveSimplexBoundFraction;

            const float caveBiomeSurfaceTemperatureOffset = temperatureNoise[columnIdx] * caveBiomeSurfaceNoiseBias;
            const float caveBiomeSurfaceHumidityOffset = humidityNoise[columnIdx] * caveBiomeSurfaceNoiseBias;

            columnLayers.clear();
            bool layerOpen = false;
            int layerStart = 0;
            CaveBiome layerBottomBiome = CaveBiome::STONE;
            CaveBiome lastSolidCaveBiome = CaveBiome::STONE;

            uint topBlockY = 0;
            bool wasSolid = true;
            for (uint y = 1; y <= maxFillY; ++y)
            {
                Block block = Block::AIR;
                CaveBiome voxelCaveBiome = CaveBiome::STONE;
                const uint blockIdx = baseBlockIdx + y;
                ASSERT(blockIdx < numChunkBlocks, "block index out of bounds");

                bool isInTerrain;
                if (y < terrainNoiseMinY)
                {
                    isInTerrain = true;
                }
                else if (y >= static_cast<uint>(terrainNoiseMaxY))
                {
                    isInTerrain = false; // surfaceVal < -surfaceValBound here, so always air; also avoids out of bounds access when maxFillY > terrainNoiseMaxY (sea fill)
                }
                else
                {
                    const int terrainNoiseIdx = baseTerrainNoiseIdx + static_cast<int>(y);
                    ASSERT(terrainNoiseIdx >= 0 && static_cast<uint>(terrainNoiseIdx) < terrainNoiseSize, "terrain noise index out of bounds");

                    float surfaceVal = (terrainBaseHeight - static_cast<float>(y)) * terrainSurfaceMultiplier;
                    if (y < terrainBaseHeight)
                    {
                        surfaceVal *= terrainBelowHeightfieldSurfaceMultiplier; // flatten terrain under base height
                    }

                    isInTerrain = terrainNoise[terrainNoiseIdx] < surfaceVal;
                }

                bool isCave = false;
                if (isInTerrain)
                {
                    if (y < static_cast<uint>(caveNoiseMaxY))
                    {
                        float caveNoiseVal;
                        if (y < caveWorleyBound)
                        {
                            const uint caveWorleyNoiseIdx = baseCaveWorleyNoiseIdx + y;
                            ASSERT(caveWorleyNoiseIdx < caveWorleyNoiseSize, "cave worley noise index out of bounds");
                            caveNoiseVal = caveNoiseWorley[caveWorleyNoiseIdx];
                        }
                        else if (y < caveSimplexBound)
                        {
                            const uint caveWorleyNoiseIdx = baseCaveWorleyNoiseIdx + y;
                            ASSERT(caveWorleyNoiseIdx < caveWorleyNoiseSize, "cave worley noise index out of bounds");
                            const float caveNoiseWorleyVal = caveNoiseWorley[caveWorleyNoiseIdx];

                            const uint caveSimplexNoiseIdx = baseCaveSimplexNoiseIdx + (y - caveSimplexNoiseMinY);
                            ASSERT(caveSimplexNoiseIdx < caveSimplexNoiseSize, "cave simplex noise index out of bounds");
                            const float caveNoiseSimplexVal = caveNoiseSimplex[caveSimplexNoiseIdx];

                            const float halfRange = (caveSimplexBound - caveWorleyBound) * 0.5f;
                            const float midpoint = caveWorleyBound + halfRange;
                            const float caveNoiseMinVal = glm::min(caveNoiseWorleyVal, caveNoiseSimplexVal);
                            if (y < midpoint)
                            {
                                const float t = (y - caveWorleyBound) / halfRange;
                                caveNoiseVal = glm::mix(caveNoiseWorleyVal, caveNoiseMinVal, glm::smoothstep(0.0f, 1.0f, t));
                            }
                            else
                            {
                                const float t = (y - midpoint) / halfRange;
                                caveNoiseVal = glm::mix(caveNoiseMinVal, caveNoiseSimplexVal, glm::smoothstep(0.0f, 1.0f, t));
                            }
                        }
                        else
                        {
                            const uint caveSimplexNoiseIdx = baseCaveSimplexNoiseIdx + (y - caveSimplexNoiseMinY);
                            ASSERT(caveSimplexNoiseIdx < caveSimplexNoiseSize, "cave simplex noise index out of bounds");
                            caveNoiseVal = caveNoiseSimplex[caveSimplexNoiseIdx];
                        }

                        float caveSurfaceVal = glm::mix(0.6f, -0.3f, glm::smoothstep(terrainBaseHeight - 20.f, terrainBaseHeight - 4.f, static_cast<float>(y)));
                        caveSurfaceVal -= glm::smoothstep(240.0f, 320.0f, static_cast<float>(y)) * 0.8f;
                        // Seal caves at and below nearby pond levels: bank columns have high base
                        // heights, so the base-relative fade above would otherwise leave caves open
                        // below a neighboring pond's waterline and expose its water sideways.
                        const int swampCaveSeal = swampCaveSealArray[columnIdx];
                        if (swampCaveSeal > 0)
                        {
                            caveSurfaceVal -= smoothstep(static_cast<float>(swampCaveSeal + 10), static_cast<float>(swampCaveSeal + 4), static_cast<float>(y)) * 1.5f;
                        }
                        isCave = caveNoiseVal < caveSurfaceVal;
                    }

                    if (!isCave)
                    {
                        Block baseBlock = Block::STONE;
                        if (y < static_cast<uint>(caveNoiseMaxY))
                        {
                            const float caveTemperature =
                                sampleCaveBiomeNoise(caveTemperatureNoise, caveBiomeNoiseSizeXZ, caveBiomeNoiseHeight, blockX, y, blockZ);
                            const float caveHumidity =
                                sampleCaveBiomeNoise(caveHumidityNoise, caveBiomeNoiseSizeXZ, caveBiomeNoiseHeight, blockX, y, blockZ);
                            const CaveBiomeNoise caveBiomeNoise = {
                                .temperature = caveTemperature + caveBiomeSurfaceTemperatureOffset,
                                .humidity = caveHumidity + caveBiomeSurfaceHumidityOffset,
                            };
                            const CaveBiome caveBiome = CaveBiomes::getClosestCaveBiome(caveBiomeNoise);
                            voxelCaveBiome = caveBiome;
                            baseBlock = CaveBiomes::getCaveBiomeData(caveBiome).baseBlock;
                        }

                        const ivec3 blockPos_WS(blockPosXZ_WS.x, y, blockPosXZ_WS.y);
                        RandomNumberGenerator rng =
                            initRng(worldSeed ^ hash(103290193), blockPos_WS.x, blockPos_WS.y, blockPos_WS.z);
                        block = rng.nextFloat() < 0.04f ? Block::LAMP : baseBlock;
                    }
                }
                else if (y <= static_cast<uint>(waterLevel))
                {
                    block = (y == static_cast<uint>(waterLevel)) ? Block::WATER_TOP : Block::WATER;
                }

                this->blocks[blockIdx] = block;

                const bool isSolid = (Blocks::getBlockData(block).type == BlockType::SOLID);

                // Capture cave air pockets (layers) bottom-up as the scan crosses floor/ceiling boundaries.
                // No new noise samples: isCave and the per-voxel cave biome are already computed above.
                if (wasSolid && isCave)
                {
                    // floor event: solid -> cave air opens a layer; start is the floor solid just below
                    layerOpen = true;
                    layerStart = static_cast<int>(y) - 1;
                    layerBottomBiome = lastSolidCaveBiome;
                }
                else if (layerOpen && !isCave)
                {
                    // pocket ends: a solid voxel caps it (ceiling, closed); anything else opens to non-cave air
                    columnLayers.push_back(CaveLayer{
                        .start = layerStart,
                        .end = static_cast<int>(y) - 1,
                        .bottomBiome = layerBottomBiome,
                        .topBiome = isSolid ? voxelCaveBiome : CaveBiome::STONE,
                        .closed = isSolid,
                    });
                    layerOpen = false;
                }

                if (isSolid)
                {
                    lastSolidCaveBiome = voxelCaveBiome;
                }

                if (wasSolid && !isSolid && !isCave)
                {
                    topBlockY = y - 1;
                }
                wasSolid = isSolid;
            }

            if (topBlockY != 0)
            {
                const bool topBlockUnderwater =
                    Blocks::getBlockData(this->blocks[baseBlockIdx + topBlockY + 1]).type == BlockType::WATER;
                for (uint y = topBlockY; y > topBlockY - 5; --y)
                {
                    const uint blockIdx = baseBlockIdx + y;
                    Block& block = this->blocks[blockIdx];
                    if (block == Block::AIR || block == Block::BEDROCK)
                    {
                        break;
                    }

                    Block newBlock = (y == topBlockY) ? topBlocks.top : topBlocks.mid;
                    if (topBlockUnderwater && newBlock == Block::GRASS_BLOCK)
                    {
                        newBlock = Block::DIRT;
                    }
                    block = newBlock;
                }
            }

            for (uint y = 1; y <= 4; ++y)
            {
                Block& block = this->blocks[baseBlockIdx + y];
                if (block == Block::AIR)
                {
                    block = (y == 4) ? Block::LAVA_TOP : Block::LAVA;
                }
            }

            heightfield[columnIdx] = topBlockY;

            // A pocket still open at the top of the scan opened upward into non-cave air (sky); close
            // it unceilinged. In practice the scan always reaches non-cave air first, so this is a guard.
            if (layerOpen)
            {
                columnLayers.push_back(CaveLayer{
                    .start = layerStart,
                    .end = static_cast<int>(maxFillY),
                    .bottomBiome = layerBottomBiome,
                    .topBiome = CaveBiome::STONE,
                    .closed = false,
                });
            }

            placeCaveStructuresForColumn(blockPosXZ_WS);
        }
    }

    const ivec2 chunkEndPosBlocksXZ_WS = chunkPosBlocksXZ_WS + static_cast<int>(chunkSizeXZ);

    for (Biome biome : biomeSet)
    {
        const BiomeData& biomeData = Biomes::getBiomeData(biome);
        for (const StructureGen& structureGen : biomeData.structureGens)
        {
            const int gridCellSideLength = static_cast<int>(structureGen.gridCellSideLength);
            const int padding = static_cast<int>(structureGen.gridCellPadding);

            ASSERT(padding < gridCellSideLength);
            ASSERT(!structureGen.variants.empty());

            const uint gridSalt = structureGen.gridSalt();

            const int halfCell = gridCellSideLength / 2;
            const int innerSide = gridCellSideLength - padding;

            const int minGridZ = MathUtil::floorDiv(chunkPosBlocksXZ_WS.y /*z*/, gridCellSideLength); // inclusive
            const int maxGridZ = MathUtil::floorDiv(chunkEndPosBlocksXZ_WS.y /*z*/ - 1, gridCellSideLength); // inclusive

            for (int gridZ = minGridZ; gridZ <= maxGridZ; ++gridZ)
            {
                // odd rows shift half a cell in x (staggered/brick layout) so candidates never share a column
                const int rowShiftX = (gridZ & 1) ? halfCell : 0;

                const int minGridX = MathUtil::floorDiv(chunkPosBlocksXZ_WS.x - rowShiftX, gridCellSideLength);
                const int maxGridX = MathUtil::floorDiv(chunkEndPosBlocksXZ_WS.x - 1 - rowShiftX, gridCellSideLength);

                for (int gridX = minGridX; gridX <= maxGridX; ++gridX)
                {
                    const ivec2 cellCornerXZ_WS(gridX * gridCellSideLength + rowShiftX, gridZ * gridCellSideLength);

                    const ivec2 candidatePosXZ_WS = gridCellCandidateXZ_WS(
                        cellCornerXZ_WS, innerSide, worldSeed ^ hash(87152059), gridSalt);

                    const ivec2 candidatePosXZ_CS = candidatePosXZ_WS - chunkPosBlocksXZ_WS;
                    if (!Chunk::isInChunkXZ(candidatePosXZ_CS))
                    {
                        continue;
                    }

                    const uint columnIdx = candidatePosXZ_CS.x + chunkSizeXZ * candidatePosXZ_CS.y /*z*/;

                    const uint candidateGroundHeight = heightfield[columnIdx];
                    if (candidateGroundHeight == 0)
                    {
                        continue; // top of this column is a cave, so skip this candidate
                    }

                    if (!bool(structureGen.flags & STRUCTURE_GEN_FLAG_ALLOW_UNDERWATER))
                    {
                        const uint blockIdx = (candidateGroundHeight + 1) + (chunkSizeY * columnIdx);
                        if (Blocks::getBlockData(this->blocks[blockIdx]).type == BlockType::WATER)
                        {
                            continue;
                        }
                    }

                    const Biome columnBiome = this->biomes[columnIdx];
                    if (columnBiome != biome)
                    {
                        continue;
                    }

                    const ivec3 candidatePos_WS = ivec3(candidatePosXZ_WS.x, candidateGroundHeight + 1, candidatePosXZ_WS.y /*z*/);
                    RandomNumberGenerator variantRng =
                        initRng(worldSeed ^ hash(1946793319), candidatePosXZ_WS.x, candidatePosXZ_WS.y /*z*/, gridSalt);
                    this->structures.emplace_back(structureGen.pickVariant(variantRng), candidatePos_WS);
                }
            }
        }
    }
}
