// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "surface_chunk_cache.h"

#include "biome.h"
#include "block.h"
#include "block_orientation.h"
#include "chunk.h"
#include "multithreading/thread_memory_allocator.h"
#include "multithreading/thread_pool.h"
#include "util/glm_util.h"
#include "util/packing.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace glm;

// Tasks queued or running at once. Each is short, so this only bounds how far ahead of the workers the
// cache commits to an order.
inline constexpr uint32_t maxTasksInFlight = 64;
// Chunks holding full blocks at once. Their buffers stay pooled afterward, so this bounds that memory.
inline constexpr uint32_t maxChunksWithTerrain = 256;
// Cells nobody requested for this many updates are dropped; regenerating them is cheap
inline constexpr uint64_t keepUnrequestedCellsUpdates = 600;
// Only this many of the highest-priority requests are ordered and considered each update; the tasks in
// flight run out well before
inline constexpr size_t maxRequestsConsidered = 1024;
// Unrequested chunks are dropped and give up their claims once every this many updates, which saves
// walking the whole cache every frame
inline constexpr uint64_t dropUnneededIntervalUpdates = 32;

VoxelCell SurfaceChunkCells::cellAt(ivec2 cellXZ, int cellY) const
{
    const int bandCellY = cellY - this->minBlockY / voxelCellSize;
    if (bandCellY < 0)
    {
        return { Block::STONE, Block::STONE, fullCellFill };
    }
    if (bandCellY >= this->numCellsY)
    {
        return {};
    }
    return this->cells[cellXZ.x + cellsPerChunkSide * (cellXZ.y + cellsPerChunkSide * bandCellY)];
}

Block SurfaceChunkCells::sideBlockAt(int sideIdx, int alongIdx, int blockY) const
{
    if (blockY < this->minBlockY)
    {
        return Block::STONE;
    }
    if (blockY >= this->getMaxBlockY())
    {
        return Block::AIR;
    }
    return this->sideBlocks[sideIdx][alongIdx + chunkSizeXZ * (blockY - this->minBlockY)];
}

bool fillsFromBottom(const BlockData& blockData)
{
    return blockData.type != BlockType::AIR && blockData.type != BlockType::WATER && !isDecoratorShape(blockData.shape);
}

int blockFill(const BlockData& blockData)
{
    return static_cast<int>(blockShapeTopHeight(blockData.shape) * fillUnitsPerBlock);
}

// Of a 2x2x2 group of blocks, bottom four first, the cell's block is the most common that fills from the
// bottom, preferring whole blocks over partial ones lying on them and going to the higher on ties, so
// canopies and thin pillars survive and surfaces keep their top blocks; else water if any; else air. The
// cell fills up to its highest column, counting a block on top as standing on a full one, so it covers at
// least the blocks it stands for and keeps their shapes' heights (a snow layer on the ground tops the
// cell an eighth of a block above it), and that column's top block shows on top.
static VoxelCell downsampleBlocks(const std::array<Block, 8>& blocks)
{
    VoxelCell cell;
    int bestCount = 0;
    bool bestIsWhole = false;
    bool hasWater = false;
    for (int i = 0; i < 8; ++i)
    {
        const BlockData& blockData = Blocks::getBlockData(blocks[i]);
        if (blockData.type == BlockType::WATER)
        {
            hasWater = true;
            continue;
        }
        if (!fillsFromBottom(blockData))
        {
            continue;
        }
        const bool isWhole = blockFill(blockData) == fillUnitsPerBlock;
        const int count = static_cast<int>(std::count(blocks.begin(), blocks.end(), blocks[i]));
        if (isWhole > bestIsWhole || (isWhole == bestIsWhole && count >= bestCount))
        {
            cell.block = blocks[i];
            bestCount = count;
            bestIsWhole = isWhole;
        }
    }
    if (cell.block == Block::AIR)
    {
        if (hasWater)
        {
            cell = { Block::WATER, Block::WATER, fullCellFill };
        }
        return cell;
    }

    int fill = 0;
    for (int column = 0; column < 4; ++column)
    {
        for (const int i : { column + 4, column })
        {
            const BlockData& blockData = Blocks::getBlockData(blocks[i]);
            if (!fillsFromBottom(blockData))
            {
                continue;
            }
            const int columnFill = (i >= 4 ? fillUnitsPerBlock : 0) + blockFill(blockData);
            if (columnFill > fill)
            {
                fill = columnFill;
                cell.topBlock = blocks[i];
            }
            break;
        }
    }
    cell.fill = static_cast<uint8_t>(fill);
    return cell;
}

// The chunk must have all its blocks
static std::unique_ptr<SurfaceChunkCells> downsampleChunk(const Chunk& chunk)
{
    auto cells = std::make_unique<SurfaceChunkCells>();
    const auto blockAt = [&](uvec2 posXZ, int blockY)
    {
        return blockY < static_cast<int>(chunkSizeY) ? chunk.getGeneratedColumn(posXZ)[blockY] : Block::AIR;
    };

    // The band spans from below the lowest solid column top (under any water) to the highest top; below it
    // is all solid
    int lowestTopY = static_cast<int>(chunkSizeY);
    int highestTopY = 0;
    for (uint32_t z = 0; z < chunkSizeXZ; ++z)
    {
        for (uint32_t x = 0; x < chunkSizeXZ; ++x)
        {
            const Block* column = chunk.getGeneratedColumn(uvec2(x, z));
            int topY = static_cast<int>(chunkSizeY) - 1;
            while (topY > 0 && column[topY] == Block::AIR)
            {
                --topY;
            }
            highestTopY = std::max(highestTopY, topY);
            while (topY > 0 && Blocks::getBlockData(column[topY]).type == BlockType::WATER)
            {
                --topY;
            }
            lowestTopY = std::min(lowestTopY, topY);
        }
    }
    cells->minBlockY = std::max(lowestTopY - 2 * voxelCellSize, 0) / voxelCellSize * voxelCellSize;
    cells->numCellsY = (highestTopY - cells->minBlockY) / voxelCellSize + 1;

    cells->cells.resize(cellsPerChunkSide * cellsPerChunkSide * cells->numCellsY);
    for (int y = 0; y < cells->numCellsY; ++y)
    {
        for (int z = 0; z < cellsPerChunkSide; ++z)
        {
            for (int x = 0; x < cellsPerChunkSide; ++x)
            {
                std::array<Block, 8> blocks;
                int waterFill = 0;
                for (int i = 0; i < 8; ++i)
                {
                    const uvec2 posXZ(x * voxelCellSize + (i & 1), z * voxelCellSize + ((i >> 1) & 1));
                    const int row = i >> 2;
                    blocks[i] = blockAt(posXZ, cells->minBlockY + y * voxelCellSize + row);
                    const BlockData& blockData = Blocks::getBlockData(blocks[i]);
                    if (blockData.type == BlockType::WATER)
                    {
                        waterFill = std::max(waterFill, row * fillUnitsPerBlock + blockFill(blockData));
                    }
                }
                VoxelCell cell = downsampleBlocks(blocks);
                cell.waterFill = static_cast<uint8_t>(waterFill);
                cells->cells[x + cellsPerChunkSide * (z + cellsPerChunkSide * y)] = cell;
            }
        }
    }

    for (int z = 0; z < cellsPerChunkSide; ++z)
    {
        for (int x = 0; x < cellsPerChunkSide; ++x)
        {
            const Biome biome = chunk.getBiomes()[x * voxelCellSize + chunkSizeXZ * (z * voxelCellSize)];
            const vec3& tint = Biomes::getBiomeData(biome).grassTint;
            cells->packedTints[x + cellsPerChunkSide * z] = Util::packUnorm8Rgb(tint.r, tint.g, tint.b);
        }
    }

    const int bandBlocksY = cells->getMaxBlockY() - cells->minBlockY;
    for (int sideIdx = 0; sideIdx < 4; ++sideIdx)
    {
        const ivec3 normal = blockFaceBases[sideIdx].normal;
        const uint32_t edge = chunkSizeXZ - 1;
        std::vector<Block>& sideBlocks = cells->sideBlocks[sideIdx];
        sideBlocks.resize(chunkSizeXZ * bandBlocksY);
        for (int y = 0; y < bandBlocksY; ++y)
        {
            for (uint32_t along = 0; along < chunkSizeXZ; ++along)
            {
                const uvec2 posXZ = normal.x != 0 ? uvec2(normal.x > 0 ? edge : 0, along)
                                                  : uvec2(along, normal.z > 0 ? edge : 0);
                sideBlocks[along + chunkSizeXZ * y] = blockAt(posXZ, cells->minBlockY + y);
            }
        }
    }
    return cells;
}

class SurfaceChunk
{
public:
    const ivec2 chunkPos;

    // Main thread only, except as noted
    // Full blocks, kept while this chunk or a neighbor has yet to fill its structures, which read them.
    // Written by the terrain task.
    std::unique_ptr<Chunk> terrain;
    // Written by the cells task
    std::unique_ptr<SurfaceChunkCells> cells;
    bool isGeneratingTerrain{ false };
    bool isGeneratingCells{ false };
    // Counted in its neighborhood's numTerrainUsers until it has its cells
    bool isWaitingForCells{ false };
    // Waiting chunks whose structure neighborhood holds this one
    uint32_t numTerrainUsers{ 0 };
    // Tiles meshing from the cells
    uint32_t numPins{ 0 };
    uint64_t lastRequestedUpdate{ 0 };
    float priority{ 0.f };
    // Position in this update's requests, which breaks priority ties
    uint32_t requestIdx{ 0 };
    // Read by the cells task
    Chunk::ConstStructureNeighborhood neighborhood{};

    explicit SurfaceChunk(ivec2 chunkPos) : chunkPos(chunkPos) {}

    bool hasTerrain() const
    {
        return this->terrain != nullptr && !this->isGeneratingTerrain;
    }
};

namespace SurfaceChunkCache
{

static std::unordered_map<ivec2, std::unique_ptr<SurfaceChunk>, glmUtil::IVec2Hash> surfaceChunks;
// This update's requests for chunks without cells
static std::vector<SurfaceChunk*> requests;
static uint64_t updateIdx{ 1 };
static uint32_t numTasksInFlight{ 0 };
static uint32_t numChunksWithTerrain{ 0 };

static std::vector<SurfaceChunk*> finishedChunks;
static std::mutex finishedChunksMutex;

static SurfaceChunk& getOrCreate(ivec2 chunkPos)
{
    std::unique_ptr<SurfaceChunk>& surfaceChunk = surfaceChunks[chunkPos];
    if (surfaceChunk == nullptr)
    {
        surfaceChunk = std::make_unique<SurfaceChunk>(chunkPos);
    }
    return *surfaceChunk;
}

// Calls func(neighbor) for each chunk in the structure neighborhood, creating any that are missing, in
// neighborhood order
template<typename Func>
static void forEachInNeighborhood(const SurfaceChunk& surfaceChunk, const Func& func)
{
    constexpr int radius = static_cast<int>(structureMaxChunkRadius);
    for (int z = -radius; z <= radius; ++z)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            func(getOrCreate(surfaceChunk.chunkPos + ivec2(x, z)));
        }
    }
}

static void freeTerrainIfUnused(SurfaceChunk& surfaceChunk)
{
    if (surfaceChunk.hasTerrain() && surfaceChunk.numTerrainUsers == 0)
    {
        surfaceChunk.terrain = nullptr;
        --numChunksWithTerrain;
    }
}

static void stopWaitingForCells(SurfaceChunk& surfaceChunk)
{
    forEachInNeighborhood(surfaceChunk,
        [](SurfaceChunk& neighbor)
        {
            --neighbor.numTerrainUsers;
            freeTerrainIfUnused(neighbor);
        });
    surfaceChunk.isWaitingForCells = false;
}

static void pushFinished(SurfaceChunk& surfaceChunk)
{
    std::scoped_lock<std::mutex> lock(finishedChunksMutex);
    finishedChunks.push_back(&surfaceChunk);
}

static void task_generateTerrain(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.surfaceChunkPtr->terrain->generateSurfaceOnlyTerrain(threadMemoryAlloc);
    pushFinished(*task.surfaceChunkPtr);
}

static void task_generateCells(const Task& task, ThreadMemoryAllocator&)
{
    SurfaceChunk& surfaceChunk = *task.surfaceChunkPtr;
    surfaceChunk.terrain->fillSurfaceOnlyStructures(surfaceChunk.neighborhood);
    surfaceChunk.cells = downsampleChunk(*surfaceChunk.terrain);
    pushFinished(surfaceChunk);
}

const SurfaceChunkCells* requestCells(ivec2 chunkPos, float priority)
{
    SurfaceChunk& surfaceChunk = getOrCreate(chunkPos);
    const bool isFirstRequest = surfaceChunk.lastRequestedUpdate != updateIdx;
    surfaceChunk.lastRequestedUpdate = updateIdx;
    if (!surfaceChunk.isGeneratingCells && surfaceChunk.cells != nullptr)
    {
        return surfaceChunk.cells.get();
    }
    if (isFirstRequest)
    {
        surfaceChunk.priority = priority;
        surfaceChunk.requestIdx = static_cast<uint32_t>(requests.size());
        requests.push_back(&surfaceChunk);
    }
    else
    {
        surfaceChunk.priority = std::min(surfaceChunk.priority, priority);
    }
    return nullptr;
}

void pinCells(ivec2 chunkPos)
{
    ++surfaceChunks.at(chunkPos)->numPins;
}

void unpinCells(ivec2 chunkPos)
{
    --surfaceChunks.at(chunkPos)->numPins;
}

static void processFinishedChunks()
{
    std::vector<SurfaceChunk*> finishedChunksNow;
    {
        std::scoped_lock<std::mutex> lock(finishedChunksMutex);
        finishedChunksNow.swap(finishedChunks);
    }
    for (SurfaceChunk* surfaceChunk : finishedChunksNow)
    {
        --numTasksInFlight;
        // A chunk generates its terrain before its cells, never both at once
        if (surfaceChunk->isGeneratingTerrain)
        {
            surfaceChunk->isGeneratingTerrain = false;
            freeTerrainIfUnused(*surfaceChunk);
        }
        else
        {
            surfaceChunk->isGeneratingCells = false;
            stopWaitingForCells(*surfaceChunk);
        }
    }
}

static void startGenerating(SurfaceChunk& surfaceChunk, void (*func)(const Task&, ThreadMemoryAllocator&),
                            std::vector<Task>& outTasks)
{
    ++numTasksInFlight;
    outTasks.push_back({ .func = func, .surfaceChunkPtr = &surfaceChunk });
}

// Highest priority first, and in request order on ties, which keeps a tile's chunks together so they share
// their neighbors' terrain. A request claims its neighborhood's terrain only once it fits under the cap on
// chunks holding terrain, so claims never pile up half generated; the first unclaimed request may exceed
// the cap, so some request always progresses.
static void startRequestedGeneration(std::vector<Task>& outTasks)
{
    const auto consideredEnd = requests.begin() + std::min(requests.size(), maxRequestsConsidered);
    std::partial_sort(requests.begin(), consideredEnd, requests.end(),
                      [](const SurfaceChunk* a, const SurfaceChunk* b)
                      {
                          return a->priority != b->priority ? a->priority < b->priority : a->requestIdx < b->requestIdx;
                      });

    bool mayExceedTerrainCap = true;
    for (auto iter = requests.begin(); iter != consideredEnd; ++iter)
    {
        SurfaceChunk* surfaceChunk = *iter;
        if (numTasksInFlight >= maxTasksInFlight)
        {
            break;
        }
        if (surfaceChunk->isGeneratingCells || surfaceChunk->cells != nullptr)
        {
            continue;
        }
        if (!surfaceChunk->isWaitingForCells)
        {
            uint32_t numMissingTerrain = 0;
            forEachInNeighborhood(*surfaceChunk,
                [&](const SurfaceChunk& neighbor) { numMissingTerrain += neighbor.terrain == nullptr ? 1 : 0; });
            if (numChunksWithTerrain + numMissingTerrain > maxChunksWithTerrain && !mayExceedTerrainCap)
            {
                continue;
            }
            mayExceedTerrainCap = false;
            forEachInNeighborhood(*surfaceChunk, [](SurfaceChunk& neighbor) { ++neighbor.numTerrainUsers; });
            surfaceChunk->isWaitingForCells = true;
        }

        bool isNeighborhoodReady = true;
        forEachInNeighborhood(*surfaceChunk,
            [&](SurfaceChunk& neighbor)
            {
                if (neighbor.hasTerrain())
                {
                    return;
                }
                isNeighborhoodReady = false;
                if (neighbor.isGeneratingTerrain || numTasksInFlight >= maxTasksInFlight)
                {
                    return;
                }
                neighbor.terrain = std::make_unique<Chunk>(neighbor.chunkPos, nullptr, true /*isSurfaceOnly*/);
                neighbor.isGeneratingTerrain = true;
                ++numChunksWithTerrain;
                startGenerating(neighbor, task_generateTerrain, outTasks);
            });
        if (!isNeighborhoodReady)
        {
            continue;
        }

        uint32_t neighborIdx = 0;
        forEachInNeighborhood(*surfaceChunk,
            [&](const SurfaceChunk& neighbor) { surfaceChunk->neighborhood[neighborIdx++] = neighbor.terrain.get(); });
        surfaceChunk->isGeneratingCells = true;
        startGenerating(*surfaceChunk, task_generateCells, outTasks);
    }
    requests.clear();
}

static void dropUnneeded()
{
    // Chunks no longer requested give up their claim on their neighbors' terrain
    for (const auto& [chunkPos, surfaceChunk] : surfaceChunks)
    {
        if (surfaceChunk->isWaitingForCells && !surfaceChunk->isGeneratingCells &&
            surfaceChunk->lastRequestedUpdate != updateIdx)
        {
            stopWaitingForCells(*surfaceChunk);
        }
    }

    for (auto iter = surfaceChunks.begin(); iter != surfaceChunks.end();)
    {
        SurfaceChunk& surfaceChunk = *iter->second;
        const bool isBusy = surfaceChunk.terrain != nullptr || surfaceChunk.isGeneratingCells ||
                            surfaceChunk.isWaitingForCells || surfaceChunk.numTerrainUsers > 0 ||
                            surfaceChunk.numPins > 0;
        if (!isBusy && (surfaceChunk.cells == nullptr ||
                        updateIdx - surfaceChunk.lastRequestedUpdate > keepUnrequestedCellsUpdates))
        {
            iter = surfaceChunks.erase(iter);
            continue;
        }
        ++iter;
    }
}

void update(std::vector<Task>& outTasks)
{
    processFinishedChunks();
    startRequestedGeneration(outTasks);
    if (updateIdx % dropUnneededIntervalUpdates == 0)
    {
        dropUnneeded();
    }
    ++updateIdx;
}

void reset()
{
    surfaceChunks.clear();
    requests.clear();
    finishedChunks.clear();
    numTasksInFlight = 0;
    numChunksWithTerrain = 0;
}

} // namespace SurfaceChunkCache
