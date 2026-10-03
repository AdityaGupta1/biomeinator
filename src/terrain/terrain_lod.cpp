// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain_lod.h"

#include "biome.h"
#include "block.h"
#include "block_orientation.h"
#include "chunk.h"
#include "chunk_generator.h"
#include "terrain_materials.h"
#include "multithreading/thread_memory_allocator.h"
#include "multithreading/thread_pool.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/cpu_profiler.h"
#include "scene/scene.h"
#include "util/glm_util.h"
#include "util/packing.h"
#include "util/rng.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace glm;

// A tile at level L covers 2^L x 2^L chunks with at most 2^maxCellsPerSideLog2 cells per side, so cells
// are single blocks up to the level where that many cells span the tile and double in size every
// level above it
inline constexpr int maxCellsPerSideLog2 = 6;
// A tile is replaced by its children within this many of its own widths of the camera, so a cell spans
// about the same angle wherever its level is shown
inline constexpr int subdivideDistanceTiles = 2;
// Tiles entirely within the render distance are only placeholders until their chunks are ready. Below
// this level there are too many of them to be worth generating, so their parents stand in.
inline constexpr int minPlaceholderLevel = 2;
// Tiles within this many chunks of the render distance's edge keep their geometry even where chunks
// cover them: moving away needs them as soon as the chunks leave, sooner than they could be generated
inline constexpr int keepGeometryMarginChunks = 4;
// Cliffs on a tile's edges reach this many cells below the lower of the two sides, so the different
// surface of a neighbor at another level never leaves a gap to see through
inline constexpr int edgeSkirtDepthCells = 4;
inline constexpr uint32_t maxGeneratingTiles = 16;

static int cellSizeLog2(int level)
{
    return std::max(0, std::countr_zero(chunkSizeXZ) + level - maxCellsPerSideLog2);
}

enum class LodTileState : uint8_t
{
    NEEDS_GEOMETRY,
    GENERATING_GEOMETRY,
    HAS_GEOMETRY,
};

class LodTile
{
public:
    // In tiles of this level
    const ivec2 tilePos;
    const int level;

    // Main thread only
    LodTileState state{ LodTileState::NEEDS_GEOMETRY };
    // Unneeded while its geometry was generating; destroyed once that finishes
    bool isMarkedForDestruction{ false };
    bool isDisplayed{ false };
    bool needsGeometry{ false };
    bool subdivides{ false };
    bool childrenRenderable{ false };
    uint64_t lastNeededFrame{ 0 };
    uint64_t lastVisitedFrame{ 0 };

    Instance* terrainInstance{ nullptr };
    Instance* waterInstance{ nullptr };

    LodTile(ivec2 tilePos, int level) : tilePos(tilePos), level(level) {}

    int getSideChunks() const
    {
        return 1 << this->level;
    }

    ivec2 getMinChunkPos() const
    {
        return this->tilePos * this->getSideChunks();
    }

    bool isReady() const
    {
        return this->state == LodTileState::HAS_GEOMETRY && this->terrainInstance->getHasBlas() &&
               (this->waterInstance == nullptr || this->waterInstance->getHasBlas());
    }

    void setVisible(bool visible)
    {
        if (this->terrainInstance != nullptr)
        {
            this->terrainInstance->setVisible(visible);
        }
        if (this->waterInstance != nullptr)
        {
            this->waterInstance->setVisible(visible);
        }
    }

    void destroyInstances(ToFreeList& toFreeList)
    {
        if (this->terrainInstance != nullptr)
        {
            toFreeList.pushInstance(this->terrainInstance);
            this->terrainInstance = nullptr;
        }
        if (this->waterInstance != nullptr)
        {
            toFreeList.pushInstance(this->waterInstance);
            this->waterInstance = nullptr;
        }
        this->state = LodTileState::NEEDS_GEOMETRY;
    }

    void createGeometry(ThreadMemoryAllocator& threadMemoryAlloc);
};

namespace TerrainLod
{

static Scene* scene;

static std::vector<LodTile*> tilesWithNewGeometry;
static std::mutex tilesWithNewGeometryMutex;

} // namespace TerrainLod

// One face of the box [boxMin, boxMax], each corner tinted by cornerTint(its local XZ)
template<typename CornerTint>
static void addQuad(HostGeometry& geometry,
                    BlockFace face,
                    vec3 boxMin,
                    vec3 boxMax,
                    const PerFaceData& faceData,
                    const CornerTint& cornerTint)
{
    const uint32_t faceIdx = blockFaceIndex(face);
    const ivec3* corners = cubeFaceVertPositions + 4 * faceIdx;
    const vec3 normal(blockFaceBases[faceIdx].normal);
    const DirectX::XMFLOAT3 normalDx{ normal.x, normal.y, normal.z };
    const uint32_t normalOct = Util::octEncode(normalDx);

    const uint32_t baseVertIdx = static_cast<uint32_t>(geometry.verts.size());
    for (uint32_t i = 0; i < 4; ++i)
    {
        const vec3 pos = boxMin + vec3(corners[i]) * (boxMax - boxMin);
        const PackedLodTerrainVertex packed =
            Util::packLodTerrainVertex({ pos.x, pos.y, pos.z }, normalDx, cornerTint(vec2(pos.x, pos.z)));
        geometry.packedTerrainVerts.push_back(std::bit_cast<PackedTerrainVertex>(packed));
        // Only the position feeds the BLAS, decoded so the traced and shaded surfaces agree
        geometry.verts.push_back({ Util::unpackLodTerrainPos(packed), normalOct, { 0.f, 0.f } });
    }
    for (const uint32_t cornerIdx : { 0u, 1u, 2u, 0u, 2u, 3u })
    {
        geometry.idxs.push_back(baseVertIdx + cornerIdx);
    }
    geometry.perFaceDatas.push_back(faceData);
}

static PerFaceData blockFaceData(Block block, BlockFace face, uint32_t extraFlags = 0)
{
    const BlockData& blockData = Blocks::getBlockData(block);
    const uint32_t texSliceIdx = blockData.texSlices[std::max(static_cast<int>(blockFaceIndex(face)) - 3, 0)];
    return TerrainMaterials::makeBlockFaceData(blockData, texSliceIdx, extraFlags);
}

// Calls emit(startX, endX, z) for each run of cells in a row that canMerge(runStart, cell) accepts
template<typename CanMerge, typename Emit>
static void forEachRowRun(int numCells, const CanMerge& canMerge, const Emit& emit)
{
    for (int z = 0; z < numCells; ++z)
    {
        int runStartX = 0;
        for (int x = 1; x <= numCells; ++x)
        {
            if (x < numCells && canMerge(ivec2(runStartX, z), ivec2(x, z)))
            {
                continue;
            }
            emit(runStartX, x, z);
            runStartX = x;
        }
    }
}

void LodTile::createGeometry(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const int cellSize = 1 << cellSizeLog2(this->level);
    const int numCells = (static_cast<int>(chunkSizeXZ) << this->level) / cellSize;
    const ivec2 originXZ_WS = this->getMinChunkPos() * static_cast<int>(chunkSizeXZ);
    const float cellSizeF = static_cast<float>(cellSize);

    // A one-sample margin gives the cliffs on the tile's edges the heights outside it
    const int numSamplesXZ = numCells + 2;
    ChunkGenerator::LodColumn* columns = threadMemoryAlloc.request<ChunkGenerator::LodColumn>(numSamplesXZ * numSamplesXZ);
    ChunkGenerator::sampleLodColumns(originXZ_WS - cellSize, cellSize, numSamplesXZ, columns, threadMemoryAlloc);
    const auto columnAt = [&](ivec2 cellPos) -> const ChunkGenerator::LodColumn&
    {
        return columns[(cellPos.x + 1) + numSamplesXZ * (cellPos.y + 1)];
    };
    const auto surfaceY = [&](ivec2 cellPos)
    {
        return static_cast<float>(columnAt(cellPos).topBlockY + 1);
    };

    // Cell corners sit on samples, so each corner takes the tint of the biome sampled there
    const auto cornerTint = [&](vec2 localXZ)
    {
        const glm::vec3& tint = Biomes::getBiomeData(columnAt(ivec2(round(localXZ / cellSizeF))).biome).grassTint;
        return Util::packUnorm8(tint.r) | (Util::packUnorm8(tint.g) << 8) | (Util::packUnorm8(tint.b) << 16);
    };

    HostGeometry& terrainGeometry = this->terrainInstance->hostGeometry;
    HostGeometry& waterGeometry = this->waterInstance->hostGeometry;

    // Tinted tops are not merged: a merged run would only carry the tints at its ends
    forEachRowRun(numCells,
        [&](ivec2 runStartPos, ivec2 cellPos)
        {
            const ChunkGenerator::LodColumn& runStart = columnAt(runStartPos);
            const ChunkGenerator::LodColumn& column = columnAt(cellPos);
            return column.topBlockY == runStart.topBlockY && column.topBlock == runStart.topBlock &&
                   !(blockFaceData(column.topBlock, BlockFace::Y_POS).getFlags() & FACE_FLAG_BIOME_TINT);
        },
        [&](int startX, int endX, int z)
        {
            const float topY = surfaceY(ivec2(startX, z));
            addQuad(terrainGeometry, BlockFace::Y_POS, vec3(startX * cellSizeF, topY, z * cellSizeF),
                    vec3(endX * cellSizeF, topY, (z + 1) * cellSizeF),
                    blockFaceData(columnAt(ivec2(startX, z)).topBlock, BlockFace::Y_POS), cornerTint);
        });

    for (int z = 0; z < numCells; ++z)
    {
        for (int x = 0; x < numCells; ++x)
        {
            const ivec2 cellPos(x, z);
            const float topY = surfaceY(cellPos);
            for (uint8_t faceIdx = 0; faceIdx < 4; ++faceIdx)
            {
                const BlockFace face = static_cast<BlockFace>(faceIdx);
                const ivec2 neighborPos = cellPos + ivec2(blockFaceBases[faceIdx].normal.x, blockFaceBases[faceIdx].normal.z);
                const float neighborTopY = surfaceY(neighborPos);
                const bool isTileEdge = glm::any(glm::lessThan(neighborPos, ivec2(0))) ||
                                        glm::any(glm::greaterThanEqual(neighborPos, ivec2(numCells)));
                const float bottomY = std::max(
                    isTileEdge ? std::min(topY, neighborTopY) - edgeSkirtDepthCells * cellSizeF : neighborTopY, 0.f);
                if (bottomY >= topY)
                {
                    continue;
                }
                addQuad(terrainGeometry, face, vec3(x * cellSizeF, bottomY, z * cellSizeF),
                        vec3((x + 1) * cellSizeF, topY, (z + 1) * cellSizeF),
                        blockFaceData(columnAt(cellPos).sideBlock, face), cornerTint);
            }
        }
    }

    const float waterTopHeight = blockShapeTopHeight(Blocks::getBlockData(Block::WATER_TOP).shape);
    const PerFaceData waterFaceData =
        blockFaceData(Block::WATER_TOP, BlockFace::Y_POS, FACE_FLAG_IS_WATER | FACE_FLAG_IS_WATER_TOP);
    const auto waterLevelAt = [&](ivec2 cellPos)
    {
        const ChunkGenerator::LodColumn& column = columnAt(cellPos);
        return column.topBlockY < column.waterLevel ? column.waterLevel : -1;
    };
    forEachRowRun(numCells,
        [&](ivec2 runStartPos, ivec2 cellPos) { return waterLevelAt(cellPos) == waterLevelAt(runStartPos); },
        [&](int startX, int endX, int z)
        {
            const ChunkGenerator::LodColumn& column = columnAt(ivec2(startX, z));
            if (column.topBlockY >= column.waterLevel)
            {
                return;
            }
            const float waterY = static_cast<float>(column.waterLevel) + waterTopHeight;
            addQuad(waterGeometry, BlockFace::Y_POS, vec3(startX * cellSizeF, waterY, z * cellSizeF),
                    vec3(endX * cellSizeF, waterY, (z + 1) * cellSizeF), waterFaceData, cornerTint);
        });

    const ivec3 transformOffset(originXZ_WS.x, 0, originXZ_WS.y /*z*/);
    this->terrainInstance->setPackedVertexFormat(VERTEX_FORMAT_PACKED_LOD_TERRAIN);
    this->terrainInstance->setTransformOffset(transformOffset);
    this->terrainInstance->setTrisPerFaceLog2(1);
    this->terrainInstance->finalizeGeometry();
    this->terrainInstance->setMaterialIdx(TerrainMaterials::getMaterialIdx(TerrainMaterial::DEFAULT));
    // Every top and side block is opaque
    this->terrainInstance->setIsOpaque(true);

    if (!waterGeometry.verts.empty())
    {
        this->waterInstance->setPackedVertexFormat(VERTEX_FORMAT_PACKED_LOD_TERRAIN);
        this->waterInstance->setTransformOffset(transformOffset);
        this->waterInstance->setTrisPerFaceLog2(1);
        this->waterInstance->finalizeGeometry();
        this->waterInstance->setMaterialIdx(TerrainMaterials::getMaterialIdx(TerrainMaterial::WATER));
    }

    std::scoped_lock<std::mutex> lock(TerrainLod::tilesWithNewGeometryMutex);
    TerrainLod::tilesWithNewGeometry.push_back(this);
}

namespace TerrainLod
{

struct TileKey
{
    ivec2 tilePos;
    int level;

    bool operator==(const TileKey&) const = default;
};

struct TileKeyHash
{
    size_t operator()(const TileKey& key) const noexcept
    {
        return hash(key.tilePos.x ^ hash(key.tilePos.y ^ hash(key.level)));
    }
};

struct IVec2Hash
{
    size_t operator()(const ivec2& v) const noexcept
    {
        return hash(v.x ^ hash(v.y));
    }
};

static std::unordered_map<TileKey, std::unique_ptr<LodTile>, TileKeyHash> tiles;
static uint32_t numGeneratingTiles{ 0 };
static uint64_t frame{ 0 };

// Rebuilt every update
static std::vector<std::pair<float, LodTile*>> generationCandidates;
static std::vector<LodTile*> displayedTiles;
static std::vector<ivec2> displayedChunkPositions;
// What the previous update showed, to hide whatever it no longer does
static std::vector<LodTile*> prevDisplayedTiles;
static std::vector<ivec2> prevDisplayedChunkPositions;

struct UpdateContext
{
    ivec2 cameraChunkPos;
    int renderDistance;
    Chunk* (*findChunk)(ivec2 chunkPos);

    // Chebyshev distances in chunks from the camera's chunk to the tile's nearest and farthest chunks
    int distanceTo(const LodTile& tile) const
    {
        const ivec2 minChunkPos = tile.getMinChunkPos();
        const ivec2 maxChunkPos = minChunkPos + tile.getSideChunks() - 1;
        const ivec2 axisDistance = max(max(minChunkPos - cameraChunkPos, cameraChunkPos - maxChunkPos), ivec2(0));
        return max(axisDistance.x, axisDistance.y);
    }

    int farthestDistanceTo(const LodTile& tile) const
    {
        const ivec2 minChunkPos = tile.getMinChunkPos();
        const ivec2 maxChunkPos = minChunkPos + tile.getSideChunks() - 1;
        const ivec2 axisDistance = max(abs(minChunkPos - cameraChunkPos), abs(maxChunkPos - cameraChunkPos));
        return max(axisDistance.x, axisDistance.y);
    }

    bool isChunkReady(ivec2 chunkPos) const
    {
        const Chunk* chunk = this->findChunk(chunkPos);
        if (chunk == nullptr || chunk->getState() != ChunkState::HAS_GEOMETRY || chunk->getIsMarkedForDestruction())
        {
            return false;
        }
        const Instance* waterInstance = chunk->getWaterInstance();
        return chunk->getTerrainInstance()->getHasBlas() && (waterInstance == nullptr || waterInstance->getHasBlas());
    }
};

void init(Scene* scene)
{
    TerrainLod::scene = scene;
}

static LodTile* findTile(ivec2 tilePos, int level)
{
    const auto tileIter = tiles.find({ tilePos, level });
    return tileIter == tiles.end() ? nullptr : tileIter->second.get();
}

static LodTile& getOrCreateTile(ivec2 tilePos, int level)
{
    std::unique_ptr<LodTile>& tile = tiles[{ tilePos, level }];
    if (tile == nullptr)
    {
        tile = std::make_unique<LodTile>(tilePos, level);
    }
    return *tile;
}

static ivec2 childTilePos(const LodTile& tile, int childIdx)
{
    return tile.tilePos * 2 + ivec2(childIdx & 1, childIdx >> 1);
}

static bool isCoveredByExisting(const LodTile& tile, const UpdateContext& ctx);

// Whether the tile's chunk (level 0) or existing children are ready to cover it, whether or not they are
// still needed
static bool areChildrenCoveredByExisting(const LodTile& tile, const UpdateContext& ctx)
{
    if (tile.level == 0)
    {
        return ctx.isChunkReady(tile.tilePos);
    }
    for (int childIdx = 0; childIdx < 4; ++childIdx)
    {
        const LodTile* child = findTile(childTilePos(tile, childIdx), tile.level - 1);
        if (child == nullptr || !isCoveredByExisting(*child, ctx))
        {
            return false;
        }
    }
    return true;
}

static bool isCoveredByExisting(const LodTile& tile, const UpdateContext& ctx)
{
    return tile.isReady() || areChildrenCoveredByExisting(tile, ctx);
}

// Visits the tiles the camera needs below this one, creating any that are missing, and returns whether
// the tile's area can be shown: by the tile itself or by its ready descendants and chunks. One tile
// that can't be shown makes every ancestor show itself instead, so coverage must not lapse.
static bool visitNeededTile(LodTile& tile, const UpdateContext& ctx)
{
    tile.lastNeededFrame = frame;
    tile.isMarkedForDestruction = false;

    const int distance = ctx.distanceTo(tile);
    tile.subdivides = distance <= ctx.renderDistance ||
                      (tile.level > 0 && distance < (subdivideDistanceTiles << tile.level));

    bool childrenRenderable = false;
    if (tile.subdivides)
    {
        if (tile.level == 0)
        {
            childrenRenderable = ctx.isChunkReady(tile.tilePos);
        }
        else
        {
            childrenRenderable = true;
            for (int childIdx = 0; childIdx < 4; ++childIdx)
            {
                // Every child is visited, so none is left out of the needed tree
                childrenRenderable &= visitNeededTile(getOrCreateTile(childTilePos(tile, childIdx), tile.level - 1), ctx);
            }
        }
    }
    else
    {
        // A tile that stopped subdividing before it was regenerated is still covered by what it showed
        childrenRenderable = areChildrenCoveredByExisting(tile, ctx);
    }
    tile.childrenRenderable = childrenRenderable;

    const bool deepInRenderDistance =
        ctx.farthestDistanceTo(tile) <= ctx.renderDistance - keepGeometryMarginChunks;
    tile.needsGeometry = !(deepInRenderDistance && (tile.level < minPlaceholderLevel || childrenRenderable));
    if (tile.needsGeometry && tile.state == LodTileState::NEEDS_GEOMETRY)
    {
        // Distance in tile widths, so the whole area gets coarse tiles before any of it is refined
        const float priority = static_cast<float>(distance) / static_cast<float>(tile.getSideChunks());
        generationCandidates.emplace_back(priority, &tile);
    }

    return tile.isReady() || childrenRenderable;
}

// Only for tiles isCoveredByExisting accepts
static void displayExisting(LodTile& tile, const UpdateContext& ctx)
{
    tile.lastVisitedFrame = frame;
    if (tile.isReady())
    {
        displayedTiles.push_back(&tile);
    }
    else if (tile.level == 0)
    {
        displayedChunkPositions.push_back(tile.tilePos);
    }
    else
    {
        for (int childIdx = 0; childIdx < 4; ++childIdx)
        {
            displayExisting(*findTile(childTilePos(tile, childIdx), tile.level - 1), ctx);
        }
    }
}

// Shows the finest level the camera wants wherever all of it is ready, so each tile is swapped for its
// children, or the other way around, in a single frame. A tile not yet regenerated keeps showing the
// finer tiles or chunks that still cover it.
static void displayNeededTile(LodTile& tile, const UpdateContext& ctx)
{
    tile.lastVisitedFrame = frame;
    const bool showChildren = tile.childrenRenderable && (tile.subdivides || !tile.isReady());
    if (!showChildren)
    {
        if (tile.isReady())
        {
            displayedTiles.push_back(&tile);
        }
        return;
    }
    if (tile.level == 0)
    {
        displayedChunkPositions.push_back(tile.tilePos);
        return;
    }
    for (int childIdx = 0; childIdx < 4; ++childIdx)
    {
        LodTile& child = *findTile(childTilePos(tile, childIdx), tile.level - 1);
        if (tile.subdivides)
        {
            displayNeededTile(child, ctx);
        }
        else
        {
            displayExisting(child, ctx);
        }
    }
}

static void applyDisplayed(const UpdateContext& ctx)
{
    for (LodTile* tile : prevDisplayedTiles)
    {
        tile->isDisplayed = false;
    }
    for (LodTile* tile : displayedTiles)
    {
        tile->isDisplayed = true;
    }
    for (LodTile* tile : prevDisplayedTiles)
    {
        tile->setVisible(tile->isDisplayed);
    }
    for (LodTile* tile : displayedTiles)
    {
        tile->setVisible(true);
    }

    const std::unordered_set<ivec2, IVec2Hash> displayedChunkSet(displayedChunkPositions.begin(),
                                                                 displayedChunkPositions.end());
    for (const ivec2 chunkPos : prevDisplayedChunkPositions)
    {
        Chunk* chunk = ctx.findChunk(chunkPos);
        if (chunk != nullptr && !displayedChunkSet.contains(chunkPos))
        {
            chunk->setInstancesVisible(false);
        }
    }
    for (const ivec2 chunkPos : displayedChunkPositions)
    {
        ctx.findChunk(chunkPos)->setInstancesVisible(true);
    }

    prevDisplayedTiles.swap(displayedTiles);
    displayedTiles.clear();
    prevDisplayedChunkPositions.swap(displayedChunkPositions);
    displayedChunkPositions.clear();
}

static void removeUnneededTiles(ToFreeList& toFreeList)
{
    for (auto tileIter = tiles.begin(); tileIter != tiles.end();)
    {
        LodTile& tile = *tileIter->second;
        if (tile.lastNeededFrame == frame || tile.lastVisitedFrame == frame)
        {
            if (!tile.needsGeometry && !tile.isDisplayed && tile.state == LodTileState::HAS_GEOMETRY)
            {
                tile.destroyInstances(toFreeList);
            }
            ++tileIter;
            continue;
        }

        if (tile.state == LodTileState::GENERATING_GEOMETRY)
        {
            tile.isMarkedForDestruction = true;
            ++tileIter;
            continue;
        }
        tile.destroyInstances(toFreeList);
        tileIter = tiles.erase(tileIter);
    }
}

static void task_createLodGeometry(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.lodTilePtr->createGeometry(threadMemoryAlloc);
}

static void startGeneratingTiles(ToFreeList& toFreeList, std::vector<Task>& outTasks)
{
    const size_t numToStart = std::min<size_t>(generationCandidates.size(), maxGeneratingTiles - numGeneratingTiles);
    // Ties go to the coarser tile
    const auto isHigherPriority = [](const std::pair<float, LodTile*>& a, const std::pair<float, LodTile*>& b)
    {
        return a.first != b.first ? a.first < b.first : a.second->level > b.second->level;
    };
    std::partial_sort(generationCandidates.begin(), generationCandidates.begin() + numToStart,
                      generationCandidates.end(), isHigherPriority);

    for (size_t i = 0; i < numToStart; ++i)
    {
        LodTile& tile = *generationCandidates[i].second;
        tile.state = LodTileState::GENERATING_GEOMETRY;
        ++numGeneratingTiles;

        tile.terrainInstance = scene->requestNewInstance(toFreeList, HostGeometrySize::LARGE);
        tile.waterInstance = scene->requestNewInstance(toFreeList, HostGeometrySize::SMALL);
        // Shown only once the tile is swapped in
        tile.setVisible(false);
        outTasks.push_back({ .func = task_createLodGeometry, .lodTilePtr = &tile });
    }
    generationCandidates.clear();
}

static void processTilesWithNewGeometry(ToFreeList& toFreeList)
{
    std::vector<LodTile*> tilesWithNewGeometryNow;
    {
        std::scoped_lock<std::mutex> lock(tilesWithNewGeometryMutex);
        tilesWithNewGeometryNow.swap(tilesWithNewGeometry);
    }

    for (LodTile* tile : tilesWithNewGeometryNow)
    {
        tile->state = LodTileState::HAS_GEOMETRY;
        --numGeneratingTiles;
        if (tile->isMarkedForDestruction)
        {
            tile->destroyInstances(toFreeList);
            tiles.erase({ tile->tilePos, tile->level });
            continue;
        }

        scene->markInstanceReadyForBlasBuild(tile->terrainInstance);
        if (tile->waterInstance->getIsGeometryFinalized())
        {
            scene->markInstanceReadyForBlasBuild(tile->waterInstance);
        }
        else
        {
            toFreeList.pushInstance(tile->waterInstance);
            tile->waterInstance = nullptr;
        }
    }
}

static int getRootLevel(int lodDistance)
{
    int level = 0;
    while ((subdivideDistanceTiles << level) < lodDistance)
    {
        ++level;
    }
    return level;
}

static void getRootTileBounds(ivec2 cameraChunkPos, int lodDistance, ivec2& outMinRootPos, ivec2& outMaxRootPos)
{
    const ivec2 rootSideChunks(1 << getRootLevel(lodDistance));
    outMinRootPos = glmUtil::floorDiv(cameraChunkPos - lodDistance, rootSideChunks);
    outMaxRootPos = glmUtil::floorDiv(cameraChunkPos + lodDistance, rootSideChunks);
}

void getCoveredChunkBounds(ivec2 cameraChunkPos, int lodDistance, ivec2& outMinChunkPos, ivec2& outMaxChunkPos)
{
    ivec2 minRootPos;
    ivec2 maxRootPos;
    getRootTileBounds(cameraChunkPos, lodDistance, minRootPos, maxRootPos);
    const int rootSideChunks = 1 << getRootLevel(lodDistance);
    outMinChunkPos = minRootPos * rootSideChunks;
    outMaxChunkPos = (maxRootPos + 1) * rootSideChunks - 1;
}

void update(ivec2 cameraChunkPos,
            int renderDistance,
            int lodDistance,
            Chunk* (*findChunk)(ivec2 chunkPos),
            ToFreeList& toFreeList,
            std::vector<Task>& outTasks)
{
    CPU_PROFILE_SCOPE("lod");
    ++frame;
    processTilesWithNewGeometry(toFreeList);

    const UpdateContext ctx{
        .cameraChunkPos = cameraChunkPos,
        .renderDistance = renderDistance,
        .findChunk = findChunk,
    };

    const int rootLevel = getRootLevel(lodDistance);
    ivec2 minRootPos;
    ivec2 maxRootPos;
    getRootTileBounds(cameraChunkPos, lodDistance, minRootPos, maxRootPos);
    std::vector<LodTile*> roots;
    for (int rootZ = minRootPos.y; rootZ <= maxRootPos.y; ++rootZ)
    {
        for (int rootX = minRootPos.x; rootX <= maxRootPos.x; ++rootX)
        {
            LodTile& root = getOrCreateTile(ivec2(rootX, rootZ), rootLevel);
            visitNeededTile(root, ctx);
            roots.push_back(&root);
        }
    }

    for (LodTile* root : roots)
    {
        displayNeededTile(*root, ctx);
    }
    applyDisplayed(ctx);
    removeUnneededTiles(toFreeList);
    startGeneratingTiles(toFreeList, outTasks);
}

void reset(ToFreeList& toFreeList)
{
    for (const auto& [key, tile] : tiles)
    {
        tile->destroyInstances(toFreeList);
    }
    tiles.clear();
    tilesWithNewGeometry.clear();
    generationCandidates.clear();
    displayedTiles.clear();
    displayedChunkPositions.clear();
    prevDisplayedTiles.clear();
    prevDisplayedChunkPositions.clear();
    numGeneratingTiles = 0;
}

} // namespace TerrainLod
