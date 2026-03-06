/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "terrain.h"

#include "biome.h"
#include "block.h"
#include "chunk.h"
#include "chunk_generator.h"
#include "terrain_materials.h"
#include "multithreading/thread_memory_allocator.h"
#include "multithreading/thread_pool.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/camera.h"
#include "structure/structure.h"
#include "util/glm_util.h"
#include "util/rng.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#define DEBUG_SINGLE_THREAD 0

inline constexpr int renderDistance = 30;
inline constexpr int createBlasDistance = renderDistance + 1;
inline constexpr int fillStructuresDistance = createBlasDistance + 1;
inline constexpr int generateTerrainDistance = fillStructuresDistance + structureMaxChunkRadius;

namespace Terrain
{

static Scene* scene;

static void task_generateTerrain(Chunk* chunk, ThreadMemoryAllocator& threadMemoryAlloc)
{
    chunk->generateTerrain(threadMemoryAlloc);
}

static void task_checkStructureNeighbors(Chunk* chunk, ThreadMemoryAllocator& threadMemoryAlloc)
{
    chunk->checkStructureNeighbors();
}

static void task_fillStructures(Chunk* chunk, ThreadMemoryAllocator& threadMemoryAlloc)
{
    chunk->fillStructuresAndDecorators();
}

static void task_generateSegments(Chunk* chunk, ThreadMemoryAllocator& threadMemoryAlloc)
{
    chunk->generateSegments(threadMemoryAlloc);
}

static void task_createInstances(Chunk* chunk, ThreadMemoryAllocator& threadMemoryAlloc)
{
    chunk->createInstances();
}

static ThreadPool threadPool;

void init(Scene* scene)
{
    Terrain::scene = scene;
    TerrainMaterials::init(scene);

    Blocks::init();
    Biomes::init();
    Structures::init();
    ChunkGenerator::init();

    threadPool.init();
}

struct IVec2Hash
{
    size_t operator()(const glm::ivec2& v) const noexcept
    {
        return hash(v.x ^ hash(v.y));
    }
};

static std::unordered_map<glm::ivec2, std::unique_ptr<Region>, IVec2Hash> regions;

static std::deque<Chunk*> chunksToGenerateTerrain;
static std::deque<Chunk*> chunksToGenerateGeometry;
static std::vector<Chunk*> chunksToCreateBlas;
static std::mutex chunksToCreateBlasMutex;
static std::vector<Chunk*> chunksToDestroy;
static std::mutex chunksToDestroyMutex;

static std::deque<Task> tasksToEnqueue;
std::vector<Task> thisFrameTasks;

void addChunkToCreateBlas(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksToCreateBlasMutex);
    chunksToCreateBlas.push_back(chunk);
}

void addChunkToDestroy(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksToDestroyMutex);
    chunksToDestroy.push_back(chunk);
}

static std::atomic_bool dirty{ true };

void setDirty()
{
    dirty.store(true, std::memory_order_release);
}

static glm::ivec2 lastChunkPos{ INT_MAX, INT_MAX };
static bool cameraUnderwater = false;
static glm::ivec3 voxelRenderBoundsMin_WS{ 0, 0, 0 };
static glm::ivec3 voxelRenderBoundsMax_WS{ 0, 0, 0 };

inline constexpr uint32_t maxTasksPerFrame = 48;
inline constexpr uint32_t maxNumGenerateTerrainTasksPerFrame = 6;

void update(ToFreeList& toFreeList)
{
    const Camera& camera = Renderer::getCamera();
    const glm::ivec3 cameraPosInt_WS = camera.getPosInt_WS();
    const glm::ivec2 currentChunkPos = glm::ivec2(cameraPosInt_WS.x, cameraPosInt_WS.z) / static_cast<int>(chunkSizeXZ);
    const glm::ivec2 minRenderChunkPos = currentChunkPos - renderDistance;
    const glm::ivec2 maxRenderChunkPos = currentChunkPos + renderDistance;

    voxelRenderBoundsMin_WS = {
        minRenderChunkPos.x * static_cast<int>(chunkSizeXZ),
        0,
        minRenderChunkPos.y * static_cast<int>(chunkSizeXZ),
    };
    voxelRenderBoundsMax_WS = {
        (maxRenderChunkPos.x + 1) * static_cast<int>(chunkSizeXZ),
        static_cast<int>(chunkSizeY),
        (maxRenderChunkPos.y + 1) * static_cast<int>(chunkSizeXZ),
    };

    cameraUnderwater = false;
    {
        if (cameraPosInt_WS.y >= 0 && cameraPosInt_WS.y < static_cast<int>(chunkSizeY))
        {
            const glm::ivec2 cameraChunkPos =
                glm::ivec2(MathUtil::floorDiv(cameraPosInt_WS.x, static_cast<int>(chunkSizeXZ)),
                           MathUtil::floorDiv(cameraPosInt_WS.z, static_cast<int>(chunkSizeXZ)));

            const glm::ivec2 regionPos = glmUtil::floorDiv(cameraChunkPos, glm::ivec2(regionSideLength));
            const auto regionIter = regions.find(regionPos);
            if (regionIter != regions.end())
            {
                const Chunk* cameraChunk = regionIter->second->getChunk(cameraChunkPos);
                const bool chunkValid = cameraChunk != nullptr && cameraChunk->getState() >= ChunkState::HAS_GEOMETRY &&
                                        !cameraChunk->getIsMarkedForDestruction();
                if (chunkValid)
                {
                    const int localX = cameraPosInt_WS.x - (cameraChunkPos.x * static_cast<int>(chunkSizeXZ));
                    const int localZ = cameraPosInt_WS.z - (cameraChunkPos.y /*z*/ * static_cast<int>(chunkSizeXZ));
                    const glm::uvec3 cameraBlockPos_CS{
                        static_cast<uint32_t>(localX),
                        static_cast<uint32_t>(cameraPosInt_WS.y),
                        static_cast<uint32_t>(localZ),
                    };

                    Block cameraBlock;
                    const bool blockIsWater = cameraChunk->tryGetBlock(cameraBlockPos_CS, cameraBlock) &&
                                              Blocks::getBlockData(cameraBlock).type == BlockType::WATER;
                    if (blockIsWater)
                    {
                        cameraUnderwater =
                            (cameraBlock == Block::WATER_TOP) ? (camera.getPosFloat_WS().y < 0.875f) : true;
                    }
                }
            }
        }
    }

    bool updateTerrain = currentChunkPos != lastChunkPos;
    if (lastChunkPos == glm::ivec2(INT_MAX, INT_MAX))
    {
        lastChunkPos = currentChunkPos;
        updateTerrain = true;
    }
    if (dirty.load(std::memory_order_acquire))
    {
        dirty.store(false, std::memory_order_release);
        updateTerrain = true;
    }

    if (updateTerrain)
    {
        const glm::ivec2 minCurrentChunkPos = currentChunkPos - generateTerrainDistance;
        const glm::ivec2 maxCurrentChunkPos = currentChunkPos + generateTerrainDistance;
        const glm::ivec2 minLastChunkPos = lastChunkPos - createBlasDistance;
        const glm::ivec2 maxLastChunkPos = lastChunkPos + createBlasDistance;

        const glm::ivec2 minChunkPos = glm::min(minCurrentChunkPos, minLastChunkPos);
        const glm::ivec2 maxChunkPos = glm::max(maxCurrentChunkPos, maxLastChunkPos);

        // this combined region logic will become a problem if I ever add teleportation (since the region could
        // become huge)
        const glm::ivec2 minRegionPos = glmUtil::floorDiv(minChunkPos, glm::ivec2(regionSideLength));
        const glm::ivec2 maxRegionPos = glmUtil::floorDiv(maxChunkPos, glm::ivec2(regionSideLength));

        uint32_t numGenerateTerrainTasksThisFrame = 0;

        for (int regionZ = minRegionPos.y; regionZ <= maxRegionPos.y; ++regionZ)
        {
            for (int regionX = minRegionPos.x; regionX <= maxRegionPos.x; ++regionX)
            {
                const glm::ivec2 regionPos = glm::ivec2(regionX, regionZ);

                const auto [regionIter, inserted] = regions.try_emplace(regionPos, nullptr);
                std::unique_ptr<Region>& regionPtr = regionIter->second;
                if (inserted) // region does not exist
                {
                    regionPtr = std::make_unique<Region>(regionPos);
                }

                if (regionPtr->getNumNeighborsSet() < 4)
                {
                    for (int neighborDirIdx = 0; neighborDirIdx < 4; ++neighborDirIdx)
                    {
                        const NeighborDirection neighborDir = static_cast<NeighborDirection>(neighborDirIdx);

                        if (regionPtr->getNeighbor(neighborDir) != nullptr)
                        {
                            continue;
                        }

                        const glm::ivec2 neighborRegionPos = regionPos + neighborOffset(neighborDir);

                        const auto [neighborRegionIter, neighborInserted] =
                            regions.try_emplace(neighborRegionPos, nullptr);
                        std::unique_ptr<Region>& neighborRegionPtr = neighborRegionIter->second;
                        if (neighborInserted) // neighbor region does not exist
                        {
                            neighborRegionPtr = std::make_unique<Region>(neighborRegionPos);
                        }

                        regionPtr->setNeighbor(neighborDir, neighborRegionPtr.get()); // also sets opposite direction
                    }
                }

                Region& region = *regionPtr;

                const glm::ivec2 minChunkPosInRegion = glm::max(region.regionPosChunks, minChunkPos);
                const glm::ivec2 maxChunkPosInRegion = glm::min(region.regionPosChunks + static_cast<int>(regionSideLength) - 1, maxChunkPos);

                for (int chunkZ = minChunkPosInRegion.y; chunkZ <= maxChunkPosInRegion.y; ++chunkZ)
                {
                    for (int chunkX = minChunkPosInRegion.x; chunkX <= maxChunkPosInRegion.x; ++chunkX)
                    {
                        const glm::ivec2 chunkPos = glm::ivec2(chunkX, chunkZ);

                        const int distToCurrentChunk = glmUtil::chebyshevDistance(chunkPos, currentChunkPos);
                        const bool inCurrentRenderDistance = distToCurrentChunk <= renderDistance;
                        const bool inCurrentCreateBlasDistance = distToCurrentChunk <= createBlasDistance;
                        const bool inCurrentFillStructuresDistance = distToCurrentChunk <= fillStructuresDistance;
                        const bool inCurrentGenerateTerrainDistance = distToCurrentChunk <= generateTerrainDistance;

                        const int distToLastChunk = glmUtil::chebyshevDistance(chunkPos, lastChunkPos);
                        const bool inLastCreateBlasDistance = distToLastChunk <= createBlasDistance;

                        if (!inCurrentGenerateTerrainDistance && !inLastCreateBlasDistance)
                        {
                            continue;
                        }

                        Chunk* chunk = region.getOrCreateChunk(chunkPos);
                        if (chunk->getNumNeighborsSet() < 4)
                        {
                            chunk->setNeighbors(true /*createNeighbors*/);
                        }

                        const ChunkState chunkState = chunk->getState();

                        if (inCurrentGenerateTerrainDistance)
                        {
                            if (chunkState == ChunkState::NEEDS_TERRAIN)
                            {
                                chunk->advanceState(ChunkState::GENERATING_TERRAIN);
                                chunksToGenerateTerrain.push_back(chunk);
                            }
                        }

                        if (inCurrentFillStructuresDistance)
                        {
                            if (chunkState == ChunkState::HAS_TERRAIN)
                            {
                                chunk->advanceState(ChunkState::AWAITING_STRUCTURE_NEIGHBORS);
                                tasksToEnqueue.push_back({ task_checkStructureNeighbors, chunk });
                            }
                            else if (chunkState == ChunkState::NEEDS_FILL_STRUCTURES)
                            {
                                chunk->advanceState(ChunkState::FILLING_STRUCTURES);
                                tasksToEnqueue.push_back({ task_fillStructures, chunk });
                            }
                            else if (chunkState == ChunkState::NEEDS_SEGMENTS)
                            {
                                chunk->advanceState(ChunkState::GENERATING_SEGMENTS);
                                tasksToEnqueue.push_back({ task_generateSegments, chunk });
                            }
                        }

                        if (inCurrentCreateBlasDistance)
                        {
                            chunk->setIsMarkedForDestruction(false);
                            chunk->setInstancesVisible(inCurrentRenderDistance);

                            if (chunkState == ChunkState::NEEDS_GEOMETRY)
                            {
                                chunk->advanceState(ChunkState::GENERATING_GEOMETRY);
                                chunksToGenerateGeometry.push_back(chunk);
                            }
                        }
                        else if (inLastCreateBlasDistance)
                        {
                            chunk->setInstancesVisible(false);

                            if (chunkState == ChunkState::GENERATING_GEOMETRY)
                            {
                                // set this chunk to be destroyed once its geometry is generated
                                chunk->setIsMarkedForDestruction();
                            }
                            else if (chunkState == ChunkState::HAS_GEOMETRY)
                            {
                                // destroy this chunk immediately (later in this function)
                                addChunkToDestroy(chunk);
                            }
                        }
                    }
                }
            }
        }

        lastChunkPos = currentChunkPos;
    }

    const uint32_t numGenerateTerrainTasksThisFrame =
        std::min(maxNumGenerateTerrainTasksPerFrame, static_cast<uint32_t>(chunksToGenerateTerrain.size()));
    for (int i = 0; i < numGenerateTerrainTasksThisFrame; ++i)
    {
        Chunk* chunk = chunksToGenerateTerrain.front();
        chunksToGenerateTerrain.pop_front();

        tasksToEnqueue.push_back({ task_generateTerrain, chunk });
    }

    while (!chunksToGenerateGeometry.empty())
    {
        Chunk* chunk = chunksToGenerateGeometry.front();
        chunksToGenerateGeometry.pop_front();

        Instance* terrainInstance = scene->requestNewInstance(toFreeList);
        Instance* waterInstance = scene->requestNewInstance(toFreeList);
        chunk->setInstances(terrainInstance, waterInstance);
        tasksToEnqueue.push_back({ task_createInstances, chunk });
    }

    if (!tasksToEnqueue.empty())
    {
        thisFrameTasks.reserve(maxTasksPerFrame);

        for (uint32_t i = 0; i < maxTasksPerFrame && !tasksToEnqueue.empty(); ++i)
        {
            thisFrameTasks.push_back(tasksToEnqueue.front());
            tasksToEnqueue.pop_front();
        }

#if DEBUG_SINGLE_THREAD
        ThreadMemoryAllocator threadMemoryAlloc{};
        for (const Task& task : thisFrameTasks)
        {
            task.func(task.chunkPtr, threadMemoryAlloc);
            threadMemoryAlloc.clear();
        }
#else
        threadPool.bulkEnqueue(thisFrameTasks.begin(), thisFrameTasks.end());
#endif

        thisFrameTasks.clear();
    }

    std::vector<Chunk*> chunksToCreateBlasNow;
    {
        std::scoped_lock<std::mutex> lock(chunksToCreateBlasMutex);
        chunksToCreateBlasNow = std::move(chunksToCreateBlas);
        chunksToCreateBlas.clear();
    }
    for (Chunk* chunk : chunksToCreateBlasNow)
    {
        ASSERT(chunk->getTerrainInstance()->getIsGeometryFinalized());
        scene->markInstanceReadyForBlasBuild(chunk->getTerrainInstance());

        chunk->cleanUnusedInstances(toFreeList);

        Instance* waterInstance = chunk->getWaterInstance();
        if (waterInstance != nullptr)
        {
            scene->markInstanceReadyForBlasBuild(waterInstance);
        }
    }

    std::vector<Chunk*> chunksToDestroyNow;
    {
        std::scoped_lock<std::mutex> lock(chunksToDestroyMutex);
        chunksToDestroyNow = std::move(chunksToDestroy);
        chunksToDestroy.clear();
    }
    for (Chunk* chunk : chunksToDestroyNow)
    {
        chunk->destroyInstances(toFreeList);
    }
}

void shutdown()
{
    threadPool.shutdown();
}

bool isCameraUnderwater()
{
    return cameraUnderwater;
}

glm::ivec3 getVoxelRenderBoundsMin_WS()
{
    return voxelRenderBoundsMin_WS;
}

glm::ivec3 getVoxelRenderBoundsMax_WS()
{
    return voxelRenderBoundsMax_WS;
}

} // namespace Terrain
