// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "terrain.h"

#include "biome.h"
#include "block.h"
#include "block_orientation.h"
#include "cave_biome.h"
#include "chunk.h"
#include "chunk_generator.h"
#include "region_file.h"
#include "terrain_lod.h"
#include "terrain_materials.h"
#include "terrain_omm.h"
#include "multithreading/thread_memory_allocator.h"
#include "multithreading/thread_pool.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/camera.h"
#include "rendering/renderer.h"
#include "rendering/water_displacer.h"
#include "settings_manager.h"
#include "rendering/cpu_profiler.h"
#include "logger.h"
#include "structure/cave_structure.h"
#include "structure/structure.h"
#include "util/file_util.h"
#include "util/glm_util.h"
#include "util/rng.h"

#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DEBUG_SINGLE_THREAD 0


namespace Terrain
{

static Scene* scene;

// Cached at Terrain::init. See knowledge/terrain/world_export_import.md (Cost containment).
static bool headless{ false };
static bool evictingRegions{ false };
static bool validatingEviction{ false };
// LOD tiles then own chunk visibility
static bool lodsEnabled{ false };

// Each task pins the regions within this many chunks of its own so none of them is removed while it
// runs. That covers every chunk the task reads or writes and, for chunks whose readiness it
// advances, every chunk that readiness depends on, since removing a region steps those back.
inline constexpr int generateTerrainPinRadius = 0;
inline constexpr int checkStructureNeighborsPinRadius = 2 * static_cast<int>(structureMaxChunkRadius);
inline constexpr int fillStructuresPinRadius = std::max(static_cast<int>(structureMaxChunkRadius), 2);
inline constexpr int generateSegmentsPinRadius = 1;
inline constexpr int createInstancesPinRadius = 1;
static_assert(checkStructureNeighborsPinRadius < static_cast<int>(regionSideLength) &&
              fillStructuresPinRadius < static_cast<int>(regionSideLength),
              "a task pins at most 2x2 regions");

// The task's chunk may be destroyed as soon as its last region is unpinned
static void unpinRegions(const Task& task)
{
    for (uint32_t i = 0; i < task.numPinnedRegions; ++i)
    {
        task.pinnedRegions[i]->unpin();
    }
}

static void task_generateTerrain(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.chunkPtr->generateTerrain(threadMemoryAlloc);
    unpinRegions(task);
}

static void task_checkStructureNeighbors(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.chunkPtr->checkStructureNeighbors();
    unpinRegions(task);
}

static void task_fillStructures(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.chunkPtr->fillStructuresAndDecorators();
    unpinRegions(task);
}

static void task_generateSegments(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.chunkPtr->generateSegments(threadMemoryAlloc);
    unpinRegions(task);
}

static void task_createInstances(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.chunkPtr->createInstances();
    unpinRegions(task);
}

static ThreadPool threadPool;

// Destroying a region's chunks took around 100 ms, so the main thread only unlinks removed regions
// and this thread destroys them
static std::vector<std::unique_ptr<Region>> regionsToDelete;
static std::mutex regionsToDeleteMutex;
static std::condition_variable_any regionsToDeleteCv;
static std::jthread regionDeleter;

static void deleteRegions(std::stop_token stopToken)
{
    // See knowledge/multithreading/thread_pool.md for why background threads yield to the render thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::vector<std::unique_ptr<Region>> regionsDeleting;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(regionsToDeleteMutex);
            if (!regionsToDeleteCv.wait(lock, stopToken, [] { return !regionsToDelete.empty(); }))
            {
                return;
            }
            regionsDeleting.swap(regionsToDelete);
        }
        regionsDeleting.clear();
    }
}

static void startRegionDeleter()
{
    regionDeleter = std::jthread(deleteRegions);
}

// Returns once every region already handed to it is destroyed
static void stopRegionDeleter()
{
    // Not started for glTF scenes, which never initialize terrain
    if (regionDeleter.joinable())
    {
        regionDeleter.request_stop();
        regionDeleter.join();
    }
}

// Indexed by Block; filled once block assets are loaded
static std::vector<BlockStateKind> blockStateKinds;

static RegionFile::Registry getRegionFileRegistry()
{
    return { blockStateKinds, &Biomes::getSurfaceStructureGens() };
}

// Validated here rather than in Decorator::addEntry: BiomeScanner shares biome registration
// but has no block metadata.
static void validateDecorators()
{
    const auto validate = [](const Decorator& decorator)
    {
        for (const DecoratorEntry& entry : decorator.getEntries())
        {
            if (entry.block == Block::AIR || !(entry.surfaces & (DECORATOR_SURFACE_WALL | DECORATOR_SURFACE_CEILING)))
            {
                continue;
            }
            const BlockData& blockData = Blocks::getBlockData(entry.block);
            ASSERT(blockData.shape == BlockShape::DECORATOR_CUSTOM &&
                   blockData.stateKind == BlockStateKind::SURFACE_MOUNT,
                   "wall/ceiling decorators require a surface-mounted custom model");
            ASSERT(blockData.upperHalf == Block::AIR, "two-tall decorators are floor-only");
        }
    };
    for (uint32_t biomeIdx = 0; biomeIdx < static_cast<uint32_t>(Biome::COUNT); ++biomeIdx)
    {
        validate(Biomes::getBiomeData(static_cast<Biome>(biomeIdx)).decorator);
    }
    for (uint32_t caveBiomeIdx = 0; caveBiomeIdx < static_cast<uint32_t>(CaveBiome::COUNT); ++caveBiomeIdx)
    {
        validate(CaveBiomes::getCaveBiomeData(static_cast<CaveBiome>(caveBiomeIdx)).decorator);
    }
}

void init(Scene* scene)
{
    Terrain::scene = scene;
    Terrain::headless = SettingsManager::isHeadless();
    Terrain::evictingRegions = SettingsManager::getAsBool("evictRegions");
    Terrain::validatingEviction = SettingsManager::getAsBool("validateEviction");
    Terrain::lodsEnabled = !Terrain::headless && SettingsManager::getAsInt("lodDistance") > 0;

    // Blocks::init() assigns the texture array slice indices that TerrainMaterials::init()
    // loads textures for
    Blocks::init();
    TerrainMaterials::init(scene);
    blockStateKinds.resize(static_cast<size_t>(Block::COUNT));
    for (size_t block = 0; block < blockStateKinds.size(); ++block)
    {
        blockStateKinds[block] = Blocks::getBlockData(static_cast<Block>(block)).stateKind;
    }

    Biomes::init();
    CaveBiomes::init();
    validateDecorators();
    Structures::init();
    CaveStructures::init();
    ChunkGenerator::init();

    TerrainLod::init(scene);

    threadPool.init();
    startRegionDeleter();
}

static std::unordered_map<glm::ivec2, std::unique_ptr<Region>, glmUtil::IVec2Hash> regions;

static glm::ivec2 cameraChunkPosition(glm::ivec3 position)
{
    return glmUtil::floorDiv(glm::ivec2(position.x, position.z), glm::ivec2(static_cast<int>(chunkSizeXZ)));
}

static glm::ivec2 chunkToRegionPos(glm::ivec2 chunkPos)
{
    return glmUtil::floorDiv(chunkPos, glm::ivec2(regionSideLength));
}

static Chunk* findChunk(glm::ivec2 chunkPos)
{
    const auto regionIter = regions.find(chunkToRegionPos(chunkPos));
    return regionIter == regions.end() ? nullptr : regionIter->second->getChunk(chunkPos);
}

static Region* getOrCreateRegion(glm::ivec2 regionPos)
{
    std::unique_ptr<Region>& region = regions[regionPos];
    if (region == nullptr)
    {
        region = std::make_unique<Region>(regionPos);
    }
    return region.get();
}

static Task makePinnedTask(void (*func)(const Task&, ThreadMemoryAllocator&), Chunk* chunk, int pinRadius)
{
    Task task{ .func = func, .chunkPtr = chunk };
    const glm::ivec2 minRegionPos = chunkToRegionPos(chunk->getChunkPos() - pinRadius);
    const glm::ivec2 maxRegionPos = chunkToRegionPos(chunk->getChunkPos() + pinRadius);
    // Created if missing: one created later would be unpinned, and could be removed while the task runs
    for (int regionZ = minRegionPos.y; regionZ <= maxRegionPos.y; ++regionZ)
    {
        for (int regionX = minRegionPos.x; regionX <= maxRegionPos.x; ++regionX)
        {
            Region* region = getOrCreateRegion(glm::ivec2(regionX, regionZ));
            region->pin();
            task.pinnedRegions[task.numPinnedRegions++] = region;
        }
    }
    return task;
}

static std::deque<Task> generateTerrainTasks;
// Their instances are requested on the main thread before they are enqueued
static std::deque<Task> createInstancesTasks;
static std::vector<Chunk*> chunksWithNewGeometry;
static std::mutex chunksWithNewGeometryMutex;
// Main thread only
static std::vector<Chunk*> chunksToDestroy;
// With LODs, chunks past the BLAS distance keep their instances while LOD shows them, until the tiles
// covering them are ready; otherwise the nearest ready ancestor tile, which can reach the camera,
// would stand in for them.
static std::unordered_set<Chunk*> lingeringChunks;
static std::vector<Chunk*> chunksToRevisit;
static std::mutex chunksToRevisitMutex;

static std::deque<Task> tasksToEnqueue;
std::vector<Task> thisFrameTasks;

StreamingStats getStreamingStats()
{
    return {
        .numWorkers = threadPool.getNumWorkers(),
        .workerBusyNanos = threadPool.getBusyNanos(),
        .taskBacklog = static_cast<uint32_t>(tasksToEnqueue.size()),
        .tasksPending = threadPool.getNumPendingTasks(),
    };
}

// Test-mode-only import-completion gate. See knowledge/terrain/world_export_import.md
// for timing/atomic-ordering rationale.
static std::atomic<uint32_t> expectedImportedChunks{ 0 };
static std::atomic<uint32_t> importedChunksEnqueuedForBlas{ 0 };
static std::atomic<bool> worldImportActive{ false };
// Protected by chunksWithNewGeometryMutex; each initial-import coordinate counts once.
static std::unordered_set<glm::ivec2, glmUtil::IVec2Hash> pendingImportedChunks;

void addChunkWithNewGeometry(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksWithNewGeometryMutex);
    chunksWithNewGeometry.push_back(chunk);
    if (headless && worldImportActive.load(std::memory_order_acquire) &&
        pendingImportedChunks.erase(chunk->getChunkPos()) != 0)
    {
        importedChunksEnqueuedForBlas.fetch_add(1, std::memory_order_relaxed);
    }
}

void addChunkToRevisit(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksToRevisitMutex);
    chunksToRevisit.push_back(chunk);
}

static std::atomic_bool dirty{ true };

void setDirty()
{
    dirty.store(true, std::memory_order_release);
}

static glm::ivec2 lastChunkPos{ INT_MAX, INT_MAX };
static bool cameraUnderwater = false;
static Biome cameraBiome = Biome::OCEAN;
static bool cameraBiomeValid = false;
static glm::ivec3 voxelRenderBoundsMin_WS{ 0, 0, 0 };
static glm::ivec3 voxelRenderBoundsMax_WS{ 0, 0, 0 };

inline constexpr uint32_t maxTasksPerFrame = 512;
// Bounds the scene's large host geometry pool, which is never freed; see knowledge/scene/instance.md
inline constexpr uint32_t maxTerrainInstancesHoldingHostGeometry = 512;
inline constexpr uint32_t maxNumGenerateTerrainTasksPerFrame = 96;

struct ChunkScanDistances
{
    int renderDistance;
    int createBlasDistance;
    int fillStructuresDistance;
    int generateTerrainDistance;
    // Measured to a region's nearest chunk; see knowledge/terrain/region_system.md
    int keepRegionDistance;
    int evictRegionDistance;
};

static constexpr ChunkScanDistances getScanDistances(int renderDistance)
{
    // One ring beyond render distance gets geometry and a BLAS.
    const int createBlasDistance = renderDistance + 1;
    // see knowledge/terrain/terrain_manager.md for why fillStructuresDistance has the
    // extra structureMaxChunkRadius term (not just +1)
    const int fillStructuresDistance = createBlasDistance + 1 + static_cast<int>(structureMaxChunkRadius);
    const int generateTerrainDistance = fillStructuresDistance + static_cast<int>(structureMaxChunkRadius);
    return {
        .renderDistance = renderDistance,
        .createBlasDistance = createBlasDistance,
        .fillStructuresDistance = fillStructuresDistance,
        .generateTerrainDistance = generateTerrainDistance,
        .keepRegionDistance = generateTerrainDistance + static_cast<int>(regionSideLength) / 2,
        // Past the ring of neighbor regions the scan creates, which would otherwise be recreated
        // and evicted again on every chunk crossing
        .evictRegionDistance = generateTerrainDistance + static_cast<int>(regionSideLength) + 1,
    };
}

static int getCreateBlasDistance()
{
    return getScanDistances(SettingsManager::getAsInt("renderDistance")).createBlasDistance;
}

static int chunkDistanceToRegion(const Region& region, glm::ivec2 chunkPos)
{
    const glm::ivec2 axisDistance =
        glm::max(glm::max(region.regionPosChunks - chunkPos, chunkPos - region.regionMaxPosChunks), glm::ivec2(0));
    return glm::max(axisDistance.x, axisDistance.y);
}

static void updateRegionStaging(glm::ivec2 cameraChunkPos, const ChunkScanDistances& distances)
{
    for (const auto& [regionPos, region] : regions)
    {
        const int distance = chunkDistanceToRegion(*region, cameraChunkPos);
        if (!evictingRegions || region->getIsImported() || distance <= distances.keepRegionDistance)
        {
            region->setIsStaged(false);
        }
        else if (distance > distances.evictRegionDistance)
        {
            region->setIsStaged(true);
        }
    }
}

// Final block hashes of generated chunks in evicted regions, until they are regenerated
static std::unordered_map<glm::ivec2, uint64_t, glmUtil::IVec2Hash> evictedChunkHashes;
static std::mutex evictedChunkHashesMutex;

static void recordEvictedChunkHash(const Chunk* chunk)
{
    if (!validatingEviction || chunk->getState() < ChunkState::HAS_ALL_BLOCKS)
    {
        return;
    }
    const uint64_t hash = chunk->hashFinalBlocks();
    std::scoped_lock<std::mutex> lock(evictedChunkHashesMutex);
    evictedChunkHashes[chunk->getChunkPos()] = hash;
}

void validateRegeneratedChunk(const Chunk* chunk)
{
    if (!validatingEviction)
    {
        return;
    }
    const uint64_t hash = chunk->hashFinalBlocks();
    std::scoped_lock<std::mutex> lock(evictedChunkHashesMutex);
    const auto hashIter = evictedChunkHashes.find(chunk->getChunkPos());
    if (hashIter == evictedChunkHashes.end())
    {
        return;
    }
    if (hashIter->second != hash)
    {
        Logger::logError("eviction validation: chunk (%d, %d) regenerated with different blocks",
                         chunk->getChunkPos().x, chunk->getChunkPos().y);
    }
    evictedChunkHashes.erase(hashIter);
}

// Only once nothing pins the region, so no task can touch its chunks or step any chunk whose
// readiness depends on them, and after the completion lists its tasks pushed to were drained
static void removeRegion(Region* region, ToFreeList& toFreeList)
{
    ASSERT(!region->getIsImported());
    constexpr int radius = static_cast<int>(structureMaxChunkRadius);
    for (const std::unique_ptr<Chunk>& chunkPtr : region->chunks)
    {
        Chunk* chunk = chunkPtr.get();
        if (chunk == nullptr)
        {
            continue;
        }

        recordEvictedChunkHash(chunk);

        if (chunk->getTerrainInstance() != nullptr)
        {
            chunk->destroyInstances(toFreeList);
        }

        for (int dirIdx = 0; dirIdx < 4; ++dirIdx)
        {
            const NeighborDirection dir = static_cast<NeighborDirection>(dirIdx);
            Chunk* neighbor = chunk->getNeighbor(dir);
            if (neighbor != nullptr && neighbor->getRegion() != region)
            {
                neighbor->onNeighborRemoved(oppositeNeighborDirection(dir));
            }
        }

        for (int offsetZ = -radius; offsetZ <= radius; ++offsetZ)
        {
            for (int offsetX = -radius; offsetX <= radius; ++offsetX)
            {
                const glm::ivec2 offset(offsetX, offsetZ);
                const glm::ivec2 neighborPos = chunk->getChunkPos() + offset;
                if (region->containsChunk(neighborPos))
                {
                    continue;
                }
                Chunk* neighbor = findChunk(neighborPos);
                if (neighbor != nullptr)
                {
                    neighbor->onStructureNeighborRemoved(-offset);
                }
            }
        }
    }

    for (int dirIdx = 0; dirIdx < 4; ++dirIdx)
    {
        const NeighborDirection dir = static_cast<NeighborDirection>(dirIdx);
        if (region->getNeighbor(dir) != nullptr)
        {
            region->clearNeighbor(dir);
        }
    }

    const auto regionIter = regions.find(region->regionPos);
    {
        std::scoped_lock<std::mutex> lock(regionsToDeleteMutex);
        regionsToDelete.push_back(std::move(regionIter->second));
    }
    regionsToDeleteCv.notify_one();
    regions.erase(regionIter);
}

// The per-chunk half of the scan: queues whatever stage the chunk's state and distance call
// for, and handles it entering or leaving the BLAS distance. A chunk is revisited on its own,
// with lastChunkPos equal to currentChunkPos, when a worker advanced its state; the full scan
// over every chunk in range is only for the camera changing chunk.
static void scheduleChunkWork(Chunk* chunk,
                              const glm::ivec2 currentChunkPos,
                              const glm::ivec2 lastChunkPos,
                              const ChunkScanDistances& distances)
{
    const int distToCurrentChunk = glmUtil::chebyshevDistance(chunk->getChunkPos(), currentChunkPos);
    const bool inCurrentRenderDistance = distToCurrentChunk <= distances.renderDistance;
    const bool inCurrentCreateBlasDistance = distToCurrentChunk <= distances.createBlasDistance;
    const bool inCurrentFillStructuresDistance = distToCurrentChunk <= distances.fillStructuresDistance;
    const bool inCurrentGenerateTerrainDistance = distToCurrentChunk <= distances.generateTerrainDistance;
    const bool inLastCreateBlasDistance =
        glmUtil::chebyshevDistance(chunk->getChunkPos(), lastChunkPos) <= distances.createBlasDistance;

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
            generateTerrainTasks.push_back(makePinnedTask(task_generateTerrain, chunk, generateTerrainPinRadius));
        }
    }

    if (inCurrentFillStructuresDistance)
    {
        if (chunkState == ChunkState::HAS_TERRAIN)
        {
            chunk->advanceState(ChunkState::AWAITING_STRUCTURE_NEIGHBORS);
            tasksToEnqueue.push_back(
                makePinnedTask(task_checkStructureNeighbors, chunk, checkStructureNeighborsPinRadius));
        }
        else if (chunkState == ChunkState::NEEDS_FILL_STRUCTURES)
        {
            chunk->advanceState(ChunkState::FILLING_STRUCTURES);
            tasksToEnqueue.push_back(makePinnedTask(task_fillStructures, chunk, fillStructuresPinRadius));
        }
        else if (chunkState == ChunkState::NEEDS_SEGMENTS)
        {
            chunk->advanceState(ChunkState::GENERATING_SEGMENTS);
            tasksToEnqueue.push_back(makePinnedTask(task_generateSegments, chunk, generateSegmentsPinRadius));
        }
    }

    if (inCurrentCreateBlasDistance)
    {
        chunk->setIsMarkedForDestruction(false);
        if (!lodsEnabled)
        {
            chunk->setInstancesVisible(inCurrentRenderDistance);
        }

        if (chunkState == ChunkState::NEEDS_GEOMETRY)
        {
            chunk->advanceState(ChunkState::GENERATING_GEOMETRY);
            createInstancesTasks.push_back(makePinnedTask(task_createInstances, chunk, createInstancesPinRadius));
        }
    }
    else if (inLastCreateBlasDistance)
    {
        if (!lodsEnabled)
        {
            chunk->setInstancesVisible(false);
        }

        if (chunkState == ChunkState::GENERATING_GEOMETRY)
        {
            // Set this chunk to be destroyed once its geometry is generated
            chunk->setIsMarkedForDestruction();
        }
        else if (chunkState == ChunkState::HAS_GEOMETRY)
        {
            if (lodsEnabled)
            {
                lingeringChunks.insert(chunk);
            }
            else
            {
                // Destroy this chunk's instances at the end of this update
                chunksToDestroy.push_back(chunk);
            }
        }
    }
}

void update(ToFreeList& toFreeList)
{
    const ChunkScanDistances distances = getScanDistances(SettingsManager::getAsInt("renderDistance"));

    const Camera& camera = Renderer::getCamera();
    const glm::ivec3 cameraPosInt_WS = camera.getPosInt_WS();
    const glm::ivec2 currentChunkPos = cameraChunkPosition(cameraPosInt_WS);
    glm::ivec2 minRenderChunkPos = currentChunkPos - distances.renderDistance;
    glm::ivec2 maxRenderChunkPos = currentChunkPos + distances.renderDistance;
    if (lodsEnabled)
    {
        // Rays that leave the terrain through LOD tiles are still within the world's volumes
        TerrainLod::getCoveredChunkBounds(currentChunkPos, SettingsManager::getAsInt("lodDistance"),
                                          minRenderChunkPos, maxRenderChunkPos);
    }

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

    // Wave displacement is sub-pixel beyond this, so far water keeps a static surface rather
    // than paying a BLAS refit per chunk per frame. The fade reaches rest height at the
    // animation distance and the animated set extends past it, so chunks are flat by the time
    // they leave the set. That set is chosen from the camera's chunk center so it only changes
    // when the camera changes chunk, with enough slack for the camera's position within the
    // chunk and for the chunk offset being its corner rather than its farthest vertex.
    constexpr float waterAnimationChunks = 24.f;
    constexpr float waterFadeChunks = 8.f;
    const float chunkSize = static_cast<float>(chunkSizeXZ);
    const float waveFadeEnd = waterAnimationChunks * chunkSize;
    const float waveFadeStart = waveFadeEnd - waterFadeChunks * chunkSize;
    const glm::vec2 cameraChunkCenterXZ_WS =
        glm::floor(glm::vec2(cameraPosInt_WS.x, cameraPosInt_WS.z) / chunkSize) * chunkSize + 0.5f * chunkSize;
    scene->setDeformableAnimation(cameraChunkCenterXZ_WS, waveFadeEnd + chunkSize * 2.5f, waveFadeStart, waveFadeEnd);

    CpuProfiler::beginScope("chunk scan");
    cameraUnderwater = false;
    cameraBiomeValid = false;
    {
        const Chunk* cameraChunk = findChunk(currentChunkPos);
        const bool chunkValid = cameraChunk != nullptr && cameraChunk->getState() >= ChunkState::HAS_GEOMETRY &&
                                !cameraChunk->getIsMarkedForDestruction();
        if (chunkValid)
        {
            const int localX = cameraPosInt_WS.x - (currentChunkPos.x * static_cast<int>(chunkSizeXZ));
            const int localZ = cameraPosInt_WS.z - (currentChunkPos.y /*z*/ * static_cast<int>(chunkSizeXZ));

            cameraBiome = cameraChunk->getBiomes()[localX + static_cast<int>(chunkSizeXZ) * localZ];
            cameraBiomeValid = true;

            if (cameraPosInt_WS.y >= 0 && cameraPosInt_WS.y < static_cast<int>(chunkSizeY))
            {
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
                    if (cameraBlock == Block::WATER_TOP)
                    {
                        const glm::vec3 cameraPosFloat_WS = camera.getPosFloat_WS();
                        const float surfaceY = 0.875f +
                            WaterDisplacer::sampleMeshWaveOffsetY(
                                glm::ivec2(cameraPosInt_WS.x, cameraPosInt_WS.z),
                                glm::vec2(cameraPosFloat_WS.x, cameraPosFloat_WS.z),
                                Renderer::getWaveTime());
                        cameraUnderwater = cameraPosFloat_WS.y < surfaceY;
                    }
                    else
                    {
                        cameraUnderwater = true;
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

    // Taken before the scan so a chunk a worker advances during it is still revisited next frame
    std::vector<Chunk*> chunksToRevisitNow;
    {
        std::scoped_lock<std::mutex> lock(chunksToRevisitMutex);
        chunksToRevisitNow = std::move(chunksToRevisit);
        chunksToRevisit.clear();
    }

    if (updateTerrain)
    {
        updateRegionStaging(currentChunkPos, distances);

        const glm::ivec2 minCurrentChunkPos = currentChunkPos - distances.generateTerrainDistance;
        const glm::ivec2 maxCurrentChunkPos = currentChunkPos + distances.generateTerrainDistance;
        const glm::ivec2 minLastChunkPos = lastChunkPos - distances.createBlasDistance;
        const glm::ivec2 maxLastChunkPos = lastChunkPos + distances.createBlasDistance;

        const glm::ivec2 minChunkPos = glm::min(minCurrentChunkPos, minLastChunkPos);
        const glm::ivec2 maxChunkPos = glm::max(maxCurrentChunkPos, maxLastChunkPos);

        // this combined region logic will become a problem if I ever add teleportation (since the region could
        // become huge)
        const glm::ivec2 minRegionPos = chunkToRegionPos(minChunkPos);
        const glm::ivec2 maxRegionPos = chunkToRegionPos(maxChunkPos);

        for (int regionZ = minRegionPos.y; regionZ <= maxRegionPos.y; ++regionZ)
        {
            for (int regionX = minRegionPos.x; regionX <= maxRegionPos.x; ++regionX)
            {
                const glm::ivec2 regionPos = glm::ivec2(regionX, regionZ);
                Region& region = *getOrCreateRegion(regionPos);

                if (region.getNumNeighborsSet() < 4)
                {
                    for (int neighborDirIdx = 0; neighborDirIdx < 4; ++neighborDirIdx)
                    {
                        const NeighborDirection neighborDir = static_cast<NeighborDirection>(neighborDirIdx);

                        if (region.getNeighbor(neighborDir) != nullptr)
                        {
                            continue;
                        }

                        Region* neighborRegion = getOrCreateRegion(regionPos + neighborOffset(neighborDir));
                        region.setNeighbor(neighborDir, neighborRegion); // also sets opposite direction
                    }
                }

                ASSERT(!region.getIsStaged(), "the scan must stay within keepRegionDistance");

                const glm::ivec2 minChunkPosInRegion = glm::max(region.regionPosChunks, minChunkPos);
                const glm::ivec2 maxChunkPosInRegion = glm::min(region.regionMaxPosChunks, maxChunkPos);

                for (int chunkZ = minChunkPosInRegion.y; chunkZ <= maxChunkPosInRegion.y; ++chunkZ)
                {
                    for (int chunkX = minChunkPosInRegion.x; chunkX <= maxChunkPosInRegion.x; ++chunkX)
                    {
                        const glm::ivec2 chunkPos = glm::ivec2(chunkX, chunkZ);

                        const bool inCurrentGenerateTerrainDistance =
                            glmUtil::chebyshevDistance(chunkPos, currentChunkPos) <= distances.generateTerrainDistance;
                        const bool inLastCreateBlasDistance =
                            glmUtil::chebyshevDistance(chunkPos, lastChunkPos) <= distances.createBlasDistance;
                        if (!inCurrentGenerateTerrainDistance && !inLastCreateBlasDistance)
                        {
                            continue;
                        }

                        Chunk* chunk = region.getOrCreateChunk(chunkPos);
                        scheduleChunkWork(chunk, currentChunkPos, lastChunkPos, distances);
                    }
                }
            }
        }

        lastChunkPos = currentChunkPos;
    }
    else
    {
        for (Chunk* chunk : chunksToRevisitNow)
        {
            if (!chunk->getRegion()->getIsStaged())
            {
                scheduleChunkWork(chunk, currentChunkPos, currentChunkPos, distances);
            }
        }
    }

    CpuProfiler::endScope(); // chunk scan
    CpuProfiler::beginScope("enqueue");
    // Geometry goes in ahead of new terrain: the pool is FIFO, so with a deep queue the heavy
    // generateTerrain tasks would otherwise starve the chunks that are one step from visible
    while (!createInstancesTasks.empty() &&
           scene->getNumInstancesHoldingHostGeometry(HostGeometrySize::LARGE) < maxTerrainInstancesHoldingHostGeometry)
    {
        const Task task = createInstancesTasks.front();
        createInstancesTasks.pop_front();

        Instance* terrainInstance = scene->requestNewInstance(toFreeList, HostGeometrySize::LARGE);
        Instance* waterInstance = scene->requestNewInstance(toFreeList, HostGeometrySize::SMALL);
        task.chunkPtr->setInstances(terrainInstance, waterInstance);
        tasksToEnqueue.push_back(task);
    }

    uint32_t numGenerateTerrainTasksThisFrame = 0;
    while (numGenerateTerrainTasksThisFrame < maxNumGenerateTerrainTasksPerFrame && !generateTerrainTasks.empty())
    {
        const Task task = generateTerrainTasks.front();
        generateTerrainTasks.pop_front();

        // The camera moved away while it waited; it is generated again if the region is ever needed
        if (task.chunkPtr->getRegion()->getIsStaged())
        {
            task.chunkPtr->setState(ChunkState::NEEDS_TERRAIN);
            unpinRegions(task);
            continue;
        }

        tasksToEnqueue.push_back(task);
        ++numGenerateTerrainTasksThisFrame;
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
            task.func(task, threadMemoryAlloc);
            threadMemoryAlloc.clear();
        }
#else
        {
            CPU_PROFILE_SCOPE("pool enqueue");
            threadPool.bulkEnqueue(thisFrameTasks.begin(), thisFrameTasks.end());
        }
#endif

        thisFrameTasks.clear();
    }

    CpuProfiler::endScope(); // enqueue
    CPU_PROFILE_SCOPE("blas mark, destroy");
    // Found before the completion lists are drained: a task pushes to them before it unpins, so
    // whatever the tasks touching these regions pushed is drained below
    std::vector<Region*> regionsToRemove;
    for (const auto& [regionPos, region] : regions)
    {
        if (region->getIsStaged() && !region->isPinned())
        {
            regionsToRemove.push_back(region.get());
        }
    }

    std::vector<Chunk*> chunksWithNewGeometryNow;
    {
        std::scoped_lock<std::mutex> lock(chunksWithNewGeometryMutex);
        chunksWithNewGeometryNow = std::move(chunksWithNewGeometry);
        chunksWithNewGeometry.clear();
    }
    for (Chunk* chunk : chunksWithNewGeometryNow)
    {
        // Advanced here rather than by the worker so that leaving range, which only the main thread
        // detects, is always seen either as GENERATING_GEOMETRY or as HAS_GEOMETRY
        chunk->advanceState(ChunkState::HAS_GEOMETRY);
        if (chunk->getIsMarkedForDestruction())
        {
            chunk->destroyInstances(toFreeList);
            continue;
        }

        ASSERT(chunk->getTerrainInstance()->getIsGeometryFinalized());
        scene->markInstanceReadyForBlasBuild(chunk->getTerrainInstance());

        chunk->cleanUnusedInstances(toFreeList);

        Instance* waterInstance = chunk->getWaterInstance();
        if (waterInstance != nullptr)
        {
            scene->markInstanceReadyForBlasBuild(waterInstance);
        }
    }

    for (Chunk* chunk : chunksToDestroy)
    {
        chunk->destroyInstances(toFreeList);
    }
    chunksToDestroy.clear();

    if (lodsEnabled)
    {
        std::vector<Task> lodTasks;
        const int voxelDistance = static_cast<int>(
            std::round(SettingsManager::getAsFloat("lodVoxelDistanceScale") * static_cast<float>(distances.renderDistance)));
        TerrainLod::update(currentChunkPos, distances.createBlasDistance, voxelDistance,
                           SettingsManager::getAsInt("lodDistance"), findChunk, toFreeList, lodTasks);
        // Ahead of the chunk backlog: LOD tiles are few and cheap, and the coarse ones are what covers
        // the world while it loads
        tasksToEnqueue.insert(tasksToEnqueue.begin(), lodTasks.begin(), lodTasks.end());

        std::erase_if(lingeringChunks, [&](Chunk* chunk) {
            if (glmUtil::chebyshevDistance(chunk->getChunkPos(), currentChunkPos) <= distances.createBlasDistance)
            {
                return true;
            }
            if (chunk->getAreInstancesVisible())
            {
                return false;
            }
            chunk->destroyInstances(toFreeList);
            return true;
        });
    }

    if (!regionsToRemove.empty())
    {
        CPU_PROFILE_SCOPE("region eviction");
        {
            std::scoped_lock<std::mutex> lock(chunksToRevisitMutex);
            std::erase_if(chunksToRevisit, [&](const Chunk* chunk) {
                return std::find(regionsToRemove.begin(), regionsToRemove.end(), chunk->getRegion()) !=
                       regionsToRemove.end();
            });
        }
        // Removing a region resets its neighbors' geometry, so lingering chunks beside it go first
        const auto isRemoved = [&](const Chunk* chunk) {
            return chunk != nullptr &&
                   std::find(regionsToRemove.begin(), regionsToRemove.end(), chunk->getRegion()) != regionsToRemove.end();
        };
        std::erase_if(lingeringChunks, [&](Chunk* chunk) {
            if (isRemoved(chunk))
            {
                return true;
            }
            for (int dirIdx = 0; dirIdx < 4; ++dirIdx)
            {
                if (isRemoved(chunk->getNeighbor(static_cast<NeighborDirection>(dirIdx))))
                {
                    chunk->destroyInstances(toFreeList);
                    return true;
                }
            }
            return false;
        });
        for (Region* region : regionsToRemove)
        {
            removeRegion(region, toFreeList);
        }
    }
}

static constexpr uint32_t worldJsonVersion = 2;

// Writes every completed region, then the manifest. Failures are logged by the writers.
static bool writeWorld(const std::filesystem::path& exportDir)
{
    const Camera& camera = Renderer::getCamera();
    const glm::ivec3 cameraPosInt = camera.getPosInt_WS();
    const glm::vec3 cameraPosFloat = camera.getPosFloat_WS();
    nlohmann::json worldJson;
    worldJson["version"] = worldJsonVersion;
    worldJson["camera"] = {
        { "posInt", { cameraPosInt.x, cameraPosInt.y, cameraPosInt.z } },
        { "posFloat", { cameraPosFloat.x, cameraPosFloat.y, cameraPosFloat.z } },
        { "phi", camera.getPhi() }, { "theta", camera.getTheta() },
    };
    worldJson["renderDistance"] = SettingsManager::getAsInt("renderDistance");
    worldJson["worldSeed"] = SettingsManager::getWorldSeed();
    worldJson["blocks"] = Blocks::blockIdNames;
    worldJson["regions"] = nlohmann::json::array();

    uint32_t totalChunks = 0;
    std::vector<SerializedChunkView> chunks;
    for (const auto& [position, region] : regions)
    {
        if (!region)
        {
            continue;
        }
        chunks.clear();
        for (const auto& chunk : region->chunks)
        {
            if (chunk && chunk->getState() >= ChunkState::HAS_ALL_BLOCKS)
            {
                chunks.push_back(chunk->getSerializedView());
            }
        }
        if (chunks.empty())
        {
            continue;
        }
        if (!RegionFile::write(exportDir / RegionFile::fileName(position), position, chunks,
                               getRegionFileRegistry()))
        {
            return false;
        }
        totalChunks += static_cast<uint32_t>(chunks.size());
        worldJson["regions"].push_back({ position.x, position.y });
    }

    // Publish the manifest last so a failed region write never advertises a complete world.
    const std::string jsonBytes = worldJson.dump();
    if (!FileUtil::writeAtomically(exportDir / "world.json", jsonBytes))
    {
        return false;
    }
    Logger::log("world export: exported %u chunks across %zu regions to %s", totalChunks,
                worldJson["regions"].size(), exportDir.generic_string().c_str());
    return true;
}

void exportWorld()
{
    const std::filesystem::path exportsDir = FileUtil::getDocumentsDir("exports");
    if (exportsDir.empty())
    {
        Logger::logError("world export: failed to get Documents directory");
        return;
    }

    // Reserve a fresh directory even if two exports occur in the same second.
    const std::string timestamp = FileUtil::getTimestampString();
    std::filesystem::path exportDir = exportsDir / timestamp;
    std::error_code error;
    for (uint32_t suffix = 1; !std::filesystem::create_directory(exportDir, error); ++suffix)
    {
        if (error)
        {
            Logger::logError("world export: %s: %s", exportDir.generic_string().c_str(), error.message().c_str());
            return;
        }
        exportDir = exportsDir / (timestamp + "_" + std::to_string(suffix));
    }

    if (!writeWorld(exportDir))
    {
        Logger::logError("world export: failed; removing %s", exportDir.generic_string().c_str());
        std::filesystem::remove_all(exportDir, error);
        if (error)
        {
            Logger::logError("world export cleanup: %s: %s", exportDir.generic_string().c_str(),
                             error.message().c_str());
        }
    }
}

static bool loadAndValidateWorldJson(const std::filesystem::path& worldJsonPath, nlohmann::json& outJson)
{
    if (!std::filesystem::exists(worldJsonPath))
    {
        Logger::logError("world import: world.json not found at %s",
                         worldJsonPath.generic_string().c_str());
        return false;
    }

    std::ifstream jsonFile(worldJsonPath);
    if (!jsonFile)
    {
        Logger::logError("world import: failed to open %s",
                         worldJsonPath.generic_string().c_str());
        return false;
    }

    try
    {
        outJson = nlohmann::json::parse(jsonFile);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        Logger::logError("world import: failed to parse %s: %s",
                         worldJsonPath.generic_string().c_str(), e.what());
        return false;
    }

    bool missingField = false;
    const auto requireField = [&](const char* name)
    {
        if (!outJson.contains(name))
        {
            Logger::logError("world import: world.json missing required field '%s'", name);
            missingField = true;
        }
    };
    requireField("version");
    requireField("camera");
    requireField("renderDistance");
    requireField("worldSeed");
    requireField("regions");
    requireField("blocks");
    if (missingField)
    {
        return false;
    }

    if (!outJson["version"].is_number_integer() || outJson["version"] != worldJsonVersion)
    {
        Logger::logError("world import: unsupported world.json version (expected %u)", worldJsonVersion);
        return false;
    }

    const auto integerInRange = [](const nlohmann::json& value, int64_t min, int64_t max)
    {
        if (!value.is_number_integer())
        {
            return false;
        }
        if (value.is_number_unsigned())
        {
            return value.get<uint64_t>() <= static_cast<uint64_t>(max) &&
                   (min <= 0 || value.get<uint64_t>() >= static_cast<uint64_t>(min));
        }
        const int64_t number = value.get<int64_t>();
        return number >= min && number <= max;
    };
    const auto worldInt = [&](const nlohmann::json& value)
    {
        return integerInRange(value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    };
    const auto finiteFloat = [](const nlohmann::json& value)
    {
        return value.is_number() && std::isfinite(value.get<float>());
    };
    const auto& camera = outJson["camera"];
    bool valid = integerInRange(outJson["worldSeed"], 0, std::numeric_limits<uint32_t>::max()) &&
                 integerInRange(outJson["renderDistance"], 1,
                                std::numeric_limits<int>::max() - getScanDistances(0).generateTerrainDistance) &&
                 outJson["regions"].is_array() && outJson["blocks"].is_array() &&
                 !outJson["blocks"].empty() && outJson["blocks"].size() <= (1u << 16) &&
                 camera.is_object() && camera.contains("posInt") && camera.contains("posFloat") &&
                 camera.contains("phi") && camera.contains("theta");
    if (valid)
    {
        valid = camera["posInt"].is_array() && camera["posInt"].size() == 3 &&
                camera["posFloat"].is_array() && camera["posFloat"].size() == 3 &&
                finiteFloat(camera["phi"]) && finiteFloat(camera["theta"]);
        if (valid)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                valid &= worldInt(camera["posInt"][axis]) && finiteFloat(camera["posFloat"][axis]);
            }
        }
        for (const auto& block : outJson["blocks"])
        {
            valid &= block.is_string();
        }
        for (const auto& region : outJson["regions"])
        {
            valid &= region.is_array() && region.size() == 2 && worldInt(region[0]) && worldInt(region[1]);
        }
    }
    if (!valid)
    {
        Logger::logError("world import: invalid metadata in %s", worldJsonPath.generic_string().c_str());
        return false;
    }

    return true;
}

// Maps each palette index in an imported world to the corresponding Block in this build.
// Unknown block ids map to MISSING so worlds survive block removals/renames.
static std::vector<Block> buildBlockRemapTable(const nlohmann::json& paletteJson)
{
    std::vector<Block> remapTable;
    remapTable.reserve(paletteJson.size());
    for (const nlohmann::json& entry : paletteJson)
    {
        const std::string blockIdName = entry.get<std::string>();
        const Block block = Blocks::fromId(blockIdName);
        if (block == Block::COUNT)
        {
            Logger::logWarning("world import: unknown block id '%s'; mapping to MISSING", blockIdName.c_str());
            remapTable.push_back(Block::MISSING);
        }
        else
        {
            remapTable.push_back(block);
        }
    }
    return remapTable;
}

struct ImportedWorld
{
    decltype(Terrain::regions) regions;
    std::unordered_set<glm::ivec2, glmUtil::IVec2Hash> pendingChunks;
    uint32_t seed{ 0 };
    uint32_t numChunks{ 0 };
    int renderDistance{ 0 };
    glm::ivec3 cameraPosInt{ 0 };
    glm::vec3 cameraPosFloat{ 0.f };
    float phi{ 0.f };
    float theta{ 0.f };
};

static std::optional<ImportedWorld> readWorld(const std::filesystem::path& worldDir)
{
    try
    {
        nlohmann::json worldJson;
        if (!loadAndValidateWorldJson(worldDir / "world.json", worldJson))
        {
            return std::nullopt;
        }
        ImportedWorld world;
        world.seed = worldJson["worldSeed"].get<uint32_t>();
        world.renderDistance = headless ? worldJson["renderDistance"].get<int>() :
                                         SettingsManager::getAsInt("renderDistance");
        const auto& cameraJson = worldJson["camera"];
        world.cameraPosInt = { cameraJson["posInt"][0].get<int>(), cameraJson["posInt"][1].get<int>(),
                               cameraJson["posInt"][2].get<int>() };
        world.cameraPosFloat = { cameraJson["posFloat"][0].get<float>(), cameraJson["posFloat"][1].get<float>(),
                                 cameraJson["posFloat"][2].get<float>() };
        world.phi = cameraJson["phi"].get<float>();
        world.theta = cameraJson["theta"].get<float>();
        const glm::ivec2 cameraChunkPos = cameraChunkPosition(world.cameraPosInt);
        const int createBlasDistance = getScanDistances(world.renderDistance).createBlasDistance;
        const std::vector<Block> blockRemap = buildBlockRemapTable(worldJson["blocks"]);

        for (const auto& entry : worldJson["regions"])
        {
            const glm::ivec2 position{ entry[0].get<int>(), entry[1].get<int>() };
            if (world.regions.contains(position))
            {
                Logger::logError("world import: duplicate region (%d, %d)", position.x, position.y);
                return std::nullopt;
            }
            auto data = RegionFile::read(worldDir / RegionFile::fileName(position), position, blockRemap,
                                         getRegionFileRegistry());
            if (!data)
            {
                return std::nullopt;
            }
            auto region = std::make_unique<Region>(position);
            region->setIsImported();
            for (auto& chunk : *data)
            {
                region->createChunk(chunk.position)->loadSerializedData(std::move(chunk.data));
                ++world.numChunks;
                if (headless && glmUtil::chebyshevDistance(chunk.position, cameraChunkPos) <= createBlasDistance)
                {
                    world.pendingChunks.insert(chunk.position);
                }
            }
            world.regions.emplace(position, std::move(region));
        }
        return world;
    }
    catch (const std::exception& error)
    {
        Logger::logError("world import: %s: %s", worldDir.generic_string().c_str(), error.what());
        return std::nullopt;
    }
}

// The replacement has been decoded and allocated before the current world is removed.
// No chunk work may run while its settings and import-completion batch are installed.
static void applyImportedWorld(ImportedWorld&& world, const std::filesystem::path& worldDir)
{
    ASSERT(regions.empty());
    SettingsManager::setWorldSeed(world.seed);
    ChunkGenerator::init();
    if (headless)
    {
        SettingsManager::setAsInt("renderDistance", world.renderDistance);
    }
    regions = std::move(world.regions);
    const uint32_t expected = static_cast<uint32_t>(world.pendingChunks.size());
    if (headless)
    {
        std::scoped_lock lock(chunksWithNewGeometryMutex);
        pendingImportedChunks = std::move(world.pendingChunks);
        expectedImportedChunks.store(expected, std::memory_order_relaxed);
        importedChunksEnqueuedForBlas.store(0, std::memory_order_relaxed);
        worldImportActive.store(true, std::memory_order_release);
    }
    Renderer::restoreCameraFromImport(world.cameraPosInt, world.cameraPosFloat, world.phi, world.theta);
    setDirty();
    Logger::log("world import: imported %u chunks across %zu regions from %s; expectedImported=%u",
                world.numChunks, regions.size(), worldDir.generic_string().c_str(), expected);
}

void importWorld()
{
    const std::string worldPathStr = SettingsManager::getAsString("world");
    if (worldPathStr.empty())
    {
        return;
    }
    auto world = readWorld(worldPathStr);
    if (!world)
    {
        exit(1);
    }
    applyImportedWorld(std::move(*world), worldPathStr);
}

// Tear down everything that holds Chunk* / region pointers so a fresh world can be
// loaded into the same Terrain. Caller is responsible for shutting the thread pool
// down first — half-running tasks holding chunk pointers would crash when the
// region map is cleared. See knowledge/terrain/world_export_import.md (`reimportWorld`).
static void resetTerrainState()
{
    Renderer::flush();
    // Replacing the world must discard lighting even if its material palette is reused.
    scene->invalidateRadianceHistory();

    ToFreeList scratchToFree;
    TerrainLod::reset(scratchToFree);
    for (const auto& [regionPos, regionPtr] : regions)
    {
        if (!regionPtr)
        {
            continue;
        }
        for (const std::unique_ptr<Chunk>& chunkPtr : regionPtr->chunks)
        {
            if (chunkPtr && chunkPtr->getTerrainInstance() != nullptr)
            {
                chunkPtr->destroyInstances(scratchToFree);
            }
        }
    }
    scratchToFree.freeAll();

    regions.clear();
    Chunk::clearBufferPool();

    generateTerrainTasks.clear();
    createInstancesTasks.clear();
    {
        std::scoped_lock<std::mutex> lock(chunksWithNewGeometryMutex);
        chunksWithNewGeometry.clear();
        pendingImportedChunks.clear();
    }
    chunksToDestroy.clear();
    lingeringChunks.clear();
    {
        std::scoped_lock<std::mutex> lock(chunksToRevisitMutex);
        chunksToRevisit.clear();
    }
    {
        std::scoped_lock<std::mutex> lock(evictedChunkHashesMutex);
        evictedChunkHashes.clear();
    }
    tasksToEnqueue.clear();
    thisFrameTasks.clear();
    lastChunkPos = { INT_MAX, INT_MAX };
    cameraUnderwater = false;
    cameraBiomeValid = false;
    dirty.store(true, std::memory_order_release);
    expectedImportedChunks.store(0, std::memory_order_relaxed);
    importedChunksEnqueuedForBlas.store(0, std::memory_order_relaxed);
    worldImportActive.store(false, std::memory_order_relaxed);
}

void reimportWorld(const std::filesystem::path& worldDir)
{
    auto world = readWorld(worldDir);
    if (!world)
    {
        Logger::logError("world reimport: failed; current world is unchanged");
        return;
    }

    threadPool.shutdown();
    // So no region destroyed after resetTerrainState empties the chunk buffer pool refills it
    stopRegionDeleter();
    resetTerrainState();
    applyImportedWorld(std::move(*world), worldDir);
    threadPool.init();
    startRegionDeleter();
}

bool pollHeadlessTerrain()
{
    if (!worldImportActive.load(std::memory_order_relaxed))
    {
        // Imported worlds retain their bounded import gate. Fresh procedural worlds
        // must finish the geometry ring before screenshot accumulation can start.
        if (SettingsManager::getAsString("world").empty())
        {
            if (lastChunkPos == glm::ivec2(INT_MAX, INT_MAX))
            {
                return false;
            }
            const int createBlasDistance = getCreateBlasDistance();
            for (int z = -createBlasDistance; z <= createBlasDistance; ++z)
            {
                for (int x = -createBlasDistance; x <= createBlasDistance; ++x)
                {
                    const Chunk* chunk = findChunk(lastChunkPos + glm::ivec2(x, z));
                    if (chunk == nullptr || chunk->getState() < ChunkState::HAS_GEOMETRY)
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    const uint32_t enqueued = importedChunksEnqueuedForBlas.load(std::memory_order_relaxed);
    const uint32_t expected = expectedImportedChunks.load(std::memory_order_relaxed);
    ASSERT(enqueued <= expected, "imported BLAS-enqueue counter exceeded expected total");
    if (enqueued >= expected)
    {
        Logger::log("world import: fully loaded, %u/%u imported chunks queued for BLAS",
                    enqueued, expected);
        worldImportActive.store(false, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void shutdown()
{
    threadPool.shutdown();
    stopRegionDeleter();
    // Here rather than in static destruction, where destroying chunks would depend on the chunk
    // buffer pool in chunk.cpp not having been destroyed yet
    regions.clear();
    Chunk::clearBufferPool();
    TerrainOmm::reset();
}

ResidencyStats getResidencyStats()
{
    ResidencyStats stats{
        .numRegions = static_cast<uint32_t>(regions.size()),
        .numChunks = 0,
        .pooledChunkBufferBytes = Chunk::getPooledBufferBytes(),
    };
    for (const auto& [regionPos, region] : regions)
    {
        for (const std::unique_ptr<Chunk>& chunk : region->chunks)
        {
            if (chunk != nullptr)
            {
                ++stats.numChunks;
                stats.chunkMemory += chunk->getMemory();
            }
        }
    }
    return stats;
}

bool isCameraUnderwater()
{
    return cameraUnderwater;
}

bool tryGetCameraBiome(Biome& outBiome)
{
    if (!cameraBiomeValid)
    {
        return false;
    }
    outBiome = cameraBiome;
    return true;
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
