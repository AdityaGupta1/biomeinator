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
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace glm;

// Tasks queued or running at once. Tasks only start once an update, and each takes about half a
// millisecond, so this must cover a frame's worth of work for every worker: at 64, a fresh load's ring
// kept two of 23 workers busy.
inline constexpr uint32_t maxTasksInFlight = 512;
// Chunks holding terrain at once, including unused terrain kept for later neighbors' structure passes.
// Downsampled chunks give their blocks back and keep only what neighbors read, about 35 KB of masks,
// heights and structures, so many more fit than chunks holding blocks, whose buffers stay pooled.
inline constexpr uint32_t maxChunksWithTerrain = 2048;
inline constexpr uint32_t maxChunksWithBlocks = 256;
// Only this many of the highest-priority requests are ordered and considered each update; the tasks in
// flight run out well before
inline constexpr size_t maxRequestsConsidered = 1024;
// Unrequested chunks are dropped and give up their claims once every this many updates, which saves
// walking the whole cache every frame
inline constexpr uint64_t dropUnneededIntervalUpdates = 32;
// Cells are only dropped this far out, as a multiple of the keep distance, so a camera turning back finds them
inline constexpr float cellsDropDistanceScale = 1.25f;

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
// cell an eighth of a block above it). Its top shows the most common of its columns' top blocks, going to
// the higher on ties: the highest column's alone put a trunk's log top over a whole tier of leaves.
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
    cell.footprint = 0;
    std::array<std::pair<Block, int>, 4> columnTops{};
    for (int column = 0; column < 4; ++column)
    {
        for (const int i : { column + 4, column })
        {
            const BlockData& blockData = Blocks::getBlockData(blocks[i]);
            if (!fillsFromBottom(blockData))
            {
                continue;
            }
            cell.footprint |= static_cast<uint8_t>(1u << column);
            const int columnFill = (i >= 4 ? fillUnitsPerBlock : 0) + blockFill(blockData);
            columnTops[column] = { blocks[i], columnFill };
            fill = std::max(fill, columnFill);
            break;
        }
    }
    int bestTopCount = 0;
    int bestTopFill = 0;
    for (const auto& [top, topFill] : columnTops)
    {
        if (topFill == 0)
        {
            continue;
        }
        const int count = static_cast<int>(std::count_if(columnTops.begin(), columnTops.end(),
            [&](const auto& other) { return other.second > 0 && other.first == top; }));
        if (count > bestTopCount || (count == bestTopCount && topFill > bestTopFill))
        {
            cell.topBlock = top;
            bestTopCount = count;
            bestTopFill = topFill;
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
            CellPlants& plants = cells->plants[x + cellsPerChunkSide * z];
            std::array<std::pair<Block, int>, voxelCellSize * voxelCellSize> plantCounts{};
            for (int blockIdx = 0; blockIdx < voxelCellSize * voxelCellSize; ++blockIdx)
            {
                const uvec2 posXZ(x * voxelCellSize + blockIdx % voxelCellSize, z * voxelCellSize + blockIdx / voxelCellSize);
                const Block* column = chunk.getGeneratedColumn(posXZ);
                int topY = static_cast<int>(chunkSizeY) - 1;
                while (topY > 0 && column[topY] == Block::AIR)
                {
                    --topY;
                }
                if (Blocks::getBlockData(column[topY]).shape != BlockShape::X_SHAPED)
                {
                    continue;
                }
                // The top of a two-tall plant stands on its lower half
                if (topY > 0 && Blocks::getBlockData(column[topY - 1]).upperHalf == column[topY])
                {
                    --topY;
                }
                const Block plant = column[topY];
                plants.blockMask |= static_cast<uint8_t>(1u << blockIdx);
                plants.baseY = static_cast<int16_t>(std::max(static_cast<int>(plants.baseY), topY));
                for (auto& [countedPlant, count] : plantCounts)
                {
                    if (count == 0 || countedPlant == plant)
                    {
                        countedPlant = plant;
                        ++count;
                        break;
                    }
                }
            }
            const auto mostCommon = std::max_element(plantCounts.begin(), plantCounts.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            plants.block = mostCommon->second > 0 ? mostCommon->first : Block::AIR;

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
    // Indexed by terrainHasBlocks, as unusedTerrainQueues is
    std::array<bool, 2> isInUnusedTerrainQueue{};
    // Whether the terrain still holds its blocks, which only its own cells task reads
    bool terrainHasBlocks{ false };
    uint64_t lastRequestedUpdate{ 0 };
    float priority{ 0.f };
    // Breaks priority ties, ordering requests around the camera so neighbors are requested together
    float sweepAngle{ 0.f };
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
static uint32_t numChunksWithBlocks{ 0 };
// Chunks whose terrain no waiting chunk claims, oldest first, without and with blocks: each cap frees from
// its own queue, or freeing room for blocks would discard compact terrain first. Neighbors requested later
// reuse it; without it, each chunk's terrain was generated four to five times. Entries go stale when their
// terrain is claimed again or gives its blocks back, and a chunk is in each queue once at a time.
static std::array<std::deque<ivec2>, 2> unusedTerrainQueues;

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

static bool isTerrainUnused(const SurfaceChunk& surfaceChunk)
{
    return surfaceChunk.hasTerrain() && surfaceChunk.numTerrainUsers == 0;
}

static void markTerrainIfUnused(SurfaceChunk& surfaceChunk)
{
    const size_t queueIdx = surfaceChunk.terrainHasBlocks ? 1 : 0;
    if (isTerrainUnused(surfaceChunk) && !surfaceChunk.isInUnusedTerrainQueue[queueIdx])
    {
        unusedTerrainQueues[queueIdx].push_back(surfaceChunk.chunkPos);
        surfaceChunk.isInUnusedTerrainQueue[queueIdx] = true;
    }
}

// Frees the least recently used unused terrain with or without blocks, and returns whether there was any
static bool freeLeastRecentlyUsedTerrain(bool withBlocks)
{
    const size_t queueIdx = withBlocks ? 1 : 0;
    std::deque<ivec2>& queue = unusedTerrainQueues[queueIdx];
    while (!queue.empty())
    {
        const auto iter = surfaceChunks.find(queue.front());
        queue.pop_front();
        if (iter == surfaceChunks.end())
        {
            continue;
        }
        SurfaceChunk& surfaceChunk = *iter->second;
        surfaceChunk.isInUnusedTerrainQueue[queueIdx] = false;
        if (isTerrainUnused(surfaceChunk) && surfaceChunk.terrainHasBlocks == withBlocks)
        {
            surfaceChunk.terrain = nullptr;
            --numChunksWithTerrain;
            if (surfaceChunk.terrainHasBlocks)
            {
                surfaceChunk.terrainHasBlocks = false;
                --numChunksWithBlocks;
            }
            return true;
        }
    }
    return false;
}

static void startWaitingForCells(SurfaceChunk& surfaceChunk)
{
    forEachInNeighborhood(surfaceChunk, [](SurfaceChunk& neighbor) { ++neighbor.numTerrainUsers; });
    surfaceChunk.isWaitingForCells = true;
}

static void stopWaitingForCells(SurfaceChunk& surfaceChunk)
{
    forEachInNeighborhood(surfaceChunk,
        [](SurfaceChunk& neighbor)
        {
            --neighbor.numTerrainUsers;
            markTerrainIfUnused(neighbor);
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
    surfaceChunk.terrain->releaseBlocks();
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
            markTerrainIfUnused(*surfaceChunk);
        }
        else
        {
            surfaceChunk->isGeneratingCells = false;
            surfaceChunk->terrainHasBlocks = false;
            --numChunksWithBlocks;
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

// Frees unused terrain until the missing terrain fits under both caps, and returns whether it does
static bool makeRoomForTerrain(uint32_t numMissingTerrain)
{
    while (true)
    {
        if (numChunksWithBlocks + numMissingTerrain > maxChunksWithBlocks)
        {
            if (!freeLeastRecentlyUsedTerrain(true))
            {
                return false;
            }
        }
        else if (numChunksWithTerrain + numMissingTerrain > maxChunksWithTerrain)
        {
            if (!freeLeastRecentlyUsedTerrain(false) && !freeLeastRecentlyUsedTerrain(true))
            {
                return false;
            }
        }
        else
        {
            return true;
        }
    }
}

// Highest priority first, by whole priority steps, and around the camera within a step, so a chunk's
// neighbors are requested soon after it and find its terrain still kept. A request keeps its claim on its
// neighborhood's terrain only if the terrain it lacks fits under the caps, after freeing unused terrain,
// so claims never pile up half generated; the first unclaimed request may exceed the caps, so some request
// always progresses.
static void startRequestedGeneration(ivec2 cameraChunkPos, std::vector<Task>& outTasks)
{
    for (SurfaceChunk* surfaceChunk : requests)
    {
        const vec2 offset(surfaceChunk->chunkPos - cameraChunkPos);
        surfaceChunk->sweepAngle = std::atan2(offset.y, offset.x);
    }
    const auto consideredEnd = requests.begin() + std::min(requests.size(), maxRequestsConsidered);
    std::partial_sort(requests.begin(), consideredEnd, requests.end(),
                      [](const SurfaceChunk* a, const SurfaceChunk* b)
                      {
                          const float aStep = std::floor(a->priority);
                          const float bStep = std::floor(b->priority);
                          return aStep != bStep ? aStep < bStep : a->sweepAngle < b->sweepAngle;
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
            // Claimed first, so freeing unused terrain to make room spares this neighborhood's
            startWaitingForCells(*surfaceChunk);
            uint32_t numMissingTerrain = 0;
            forEachInNeighborhood(*surfaceChunk,
                [&](const SurfaceChunk& neighbor) { numMissingTerrain += neighbor.terrain == nullptr ? 1 : 0; });
            if (!makeRoomForTerrain(numMissingTerrain) && !mayExceedTerrainCap)
            {
                stopWaitingForCells(*surfaceChunk);
                continue;
            }
            mayExceedTerrainCap = false;
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
                neighbor.terrainHasBlocks = true;
                ++numChunksWithTerrain;
                ++numChunksWithBlocks;
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

static void dropUnneeded(ivec2 cameraChunkPos, int keepDistance)
{
    const int cellsDropDistance = static_cast<int>(static_cast<float>(keepDistance) * cellsDropDistanceScale);
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
                            surfaceChunk.numPins > 0 || surfaceChunk.lastRequestedUpdate == updateIdx;
        if (!isBusy && (surfaceChunk.cells == nullptr ||
                        glmUtil::chebyshevDistance(surfaceChunk.chunkPos, cameraChunkPos) > cellsDropDistance))
        {
            iter = surfaceChunks.erase(iter);
            continue;
        }
        ++iter;
    }
}

void update(ivec2 cameraChunkPos, int keepDistance, std::vector<Task>& outTasks)
{
    processFinishedChunks();
    startRequestedGeneration(cameraChunkPos, outTasks);
    if (updateIdx % dropUnneededIntervalUpdates == 0)
    {
        dropUnneeded(cameraChunkPos, keepDistance);
    }
    ++updateIdx;
}

void reset()
{
    surfaceChunks.clear();
    requests.clear();
    finishedChunks.clear();
    unusedTerrainQueues = {};
    numTasksInFlight = 0;
    numChunksWithTerrain = 0;
    numChunksWithBlocks = 0;
}

} // namespace SurfaceChunkCache
