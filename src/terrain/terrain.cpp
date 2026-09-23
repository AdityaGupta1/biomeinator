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
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#define DEBUG_SINGLE_THREAD 0


namespace Terrain
{

static Scene* scene;

// Cached at Terrain::init. See knowledge/terrain/world_export_import.md (Cost containment).
static bool headless{ false };

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
    Terrain::headless = SettingsManager::isHeadless();

    // Blocks::init() assigns the texture array slice indices that TerrainMaterials::init()
    // loads textures for
    Blocks::init();
    TerrainMaterials::init(scene);

    Biomes::init();
    CaveBiomes::init();
    Structures::init();
    CaveStructures::init();
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

void addChunkToCreateBlas(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksToCreateBlasMutex);
    chunksToCreateBlas.push_back(chunk);
    if (headless && worldImportActive.load(std::memory_order_acquire) && chunk->getWasImported())
    {
        importedChunksEnqueuedForBlas.fetch_add(1, std::memory_order_relaxed);
    }
}

void addChunkToDestroy(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksToDestroyMutex);
    chunksToDestroy.push_back(chunk);
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
inline constexpr uint32_t maxNumGenerateTerrainTasksPerFrame = 96;

struct ChunkScanDistances
{
    int renderDistance;
    int createBlasDistance;
    int fillStructuresDistance;
    int generateTerrainDistance;
};

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
            // Set this chunk to be destroyed once its geometry is generated
            chunk->setIsMarkedForDestruction();
        }
        else if (chunkState == ChunkState::HAS_GEOMETRY)
        {
            // Destroy this chunk immediately (later in this function)
            addChunkToDestroy(chunk);
        }
    }
}

void update(ToFreeList& toFreeList)
{
    const int renderDistance = SettingsManager::getAsInt("renderDistance");
    const int createBlasDistance = renderDistance + 1;
    // see knowledge/terrain/terrain_manager.md for why fillStructuresDistance has the
    // extra structureMaxChunkRadius term (not just +1)
    const int fillStructuresDistance = createBlasDistance + 1 + structureMaxChunkRadius;
    const int generateTerrainDistance = fillStructuresDistance + structureMaxChunkRadius;

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
    }

    const ChunkScanDistances distances = {
        .renderDistance = renderDistance,
        .createBlasDistance = createBlasDistance,
        .fillStructuresDistance = fillStructuresDistance,
        .generateTerrainDistance = generateTerrainDistance,
    };

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

                        const bool inCurrentGenerateTerrainDistance =
                            glmUtil::chebyshevDistance(chunkPos, currentChunkPos) <= generateTerrainDistance;
                        const bool inLastCreateBlasDistance =
                            glmUtil::chebyshevDistance(chunkPos, lastChunkPos) <= createBlasDistance;
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
            scheduleChunkWork(chunk, currentChunkPos, currentChunkPos, distances);
        }
    }

    CpuProfiler::endScope(); // chunk scan
    CpuProfiler::beginScope("enqueue");
    // Geometry goes in ahead of new terrain: the pool is FIFO, so with a deep queue the heavy
    // generateTerrain tasks would otherwise starve the chunks that are one step from visible
    while (!chunksToGenerateGeometry.empty())
    {
        Chunk* chunk = chunksToGenerateGeometry.front();
        chunksToGenerateGeometry.pop_front();

        Instance* terrainInstance = scene->requestNewInstance(toFreeList);
        Instance* waterInstance = scene->requestNewInstance(toFreeList);
        chunk->setInstances(terrainInstance, waterInstance);
        tasksToEnqueue.push_back({ task_createInstances, chunk });
    }

    const uint32_t numGenerateTerrainTasksThisFrame =
        std::min(maxNumGenerateTerrainTasksPerFrame, static_cast<uint32_t>(chunksToGenerateTerrain.size()));
    for (int i = 0; i < numGenerateTerrainTasksThisFrame; ++i)
    {
        Chunk* chunk = chunksToGenerateTerrain.front();
        chunksToGenerateTerrain.pop_front();

        tasksToEnqueue.push_back({ task_generateTerrain, chunk });
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
        {
            CPU_PROFILE_SCOPE("pool enqueue");
            threadPool.bulkEnqueue(thisFrameTasks.begin(), thisFrameTasks.end());
        }
#endif

        thisFrameTasks.clear();
    }

    CpuProfiler::endScope(); // enqueue
    CPU_PROFILE_SCOPE("blas mark, destroy");
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

static constexpr uint32_t worldJsonVersion = 2;

void exportWorld()
{
    try
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
        for (uint32_t suffix = 1; !std::filesystem::create_directory(exportDir); ++suffix)
            exportDir = exportsDir / (timestamp + "_" + std::to_string(suffix));

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
        for (const auto& [position, region] : regions)
        {
            if (!region || std::none_of(region->chunks.begin(), region->chunks.end(), [](const auto& chunk) {
                    return chunk && chunk->getState() >= ChunkState::HAS_ALL_BLOCKS;
                })) continue;
            uint32_t chunksWritten = 0;
            if (!RegionFile::write(exportDir / RegionFile::fileName(position), *region, chunksWritten))
            {
                Logger::logError("world export: aborted; incomplete export has no world.json");
                return;
            }
            totalChunks += chunksWritten;
            worldJson["regions"].push_back({ position.x, position.y });
        }
        // Publish the manifest last so a failed region write never advertises a complete world.
        const std::string jsonBytes = worldJson.dump();
        if (!FileUtil::writeAtomically(exportDir / "world.json", jsonBytes)) return;
        Logger::log("world export: exported %u chunks across %zu regions to %s", totalChunks,
                    worldJson["regions"].size(), exportDir.generic_string().c_str());
    }
    catch (const std::exception& error)
    {
        Logger::logError("world export: %s", error.what());
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

    const auto integerInRange = [](const nlohmann::json& value, int64_t min, int64_t max) {
        if (!value.is_number_integer()) return false;
        if (value.is_number_unsigned())
            return value.get<uint64_t>() <= static_cast<uint64_t>(max) &&
                   (min <= 0 || value.get<uint64_t>() >= static_cast<uint64_t>(min));
        const int64_t number = value.get<int64_t>();
        return number >= min && number <= max;
    };
    const auto worldInt = [&](const nlohmann::json& value) {
        return integerInRange(value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    };
    const auto finiteFloat = [](const nlohmann::json& value) {
        return value.is_number() && std::isfinite(value.get<float>());
    };
    const auto& camera = outJson["camera"];
    bool valid = integerInRange(outJson["worldSeed"], 0, std::numeric_limits<uint32_t>::max()) &&
                 integerInRange(outJson["renderDistance"], 1, std::numeric_limits<int>::max() - 4) &&
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
            for (int axis = 0; axis < 3; ++axis)
                valid &= worldInt(camera["posInt"][axis]) && finiteFloat(camera["posFloat"][axis]);
        for (const auto& block : outJson["blocks"]) valid &= block.is_string();
        for (const auto& region : outJson["regions"])
            valid &= region.is_array() && region.size() == 2 && worldInt(region[0]) && worldInt(region[1]);
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

static bool importWorldImpl(const std::filesystem::path& worldDir)
{
    try
    {
        nlohmann::json worldJson;
        if (!loadAndValidateWorldJson(worldDir / "world.json", worldJson)) return false;
        if (!regions.empty())
        {
            Logger::logError("world import: terrain must be reset before replacing a world");
            return false;
        }
        const uint32_t worldSeed = worldJson["worldSeed"].get<uint32_t>();
        const int renderDistance = headless ? worldJson["renderDistance"].get<int>() : SettingsManager::getAsInt("renderDistance");
        const auto& cameraJson = worldJson["camera"];
        const glm::ivec3 cameraPosInt{ cameraJson["posInt"][0].get<int>(), cameraJson["posInt"][1].get<int>(),
                                       cameraJson["posInt"][2].get<int>() };
        const glm::vec3 cameraPosFloat{ cameraJson["posFloat"][0].get<float>(), cameraJson["posFloat"][1].get<float>(),
                                        cameraJson["posFloat"][2].get<float>() };
        const float phi = cameraJson["phi"].get<float>();
        const float theta = cameraJson["theta"].get<float>();
        const glm::ivec2 cameraChunkPos = glmUtil::floorDiv(glm::ivec2(cameraPosInt.x, cameraPosInt.z),
                                                          glm::ivec2(static_cast<int>(chunkSizeXZ)));
        const int createBlasDistance = renderDistance + 1;
        const std::vector<Block> blockRemap = buildBlockRemapTable(worldJson["blocks"]);

        // Decode into private ownership. A failed region leaves no partial world or changed seed.
        decltype(regions) loadedRegions;
        uint32_t totalChunks = 0;
        uint32_t chunksWithinBlasDistance = 0;
        for (const auto& entry : worldJson["regions"])
        {
            const glm::ivec2 position{ entry[0].get<int>(), entry[1].get<int>() };
            if (loadedRegions.contains(position))
            {
                Logger::logError("world import: duplicate region (%d, %d)", position.x, position.y);
                return false;
            }
            auto region = RegionFile::read(worldDir / RegionFile::fileName(position), position, blockRemap);
            if (!region) return false;
            for (const auto& chunk : region->chunks)
            {
                if (!chunk) continue;
                ++totalChunks;
                if (glmUtil::chebyshevDistance(chunk->getChunkPos(), cameraChunkPos) <= createBlasDistance)
                    ++chunksWithinBlasDistance;
            }
            loadedRegions.emplace(position, std::move(region));
        }

        SettingsManager::setWorldSeed(worldSeed);
        // Fresh boundary generation must use the imported seed and its cached noise offsets.
        ChunkGenerator::init();
        if (headless) SettingsManager::setAsInt("renderDistance", renderDistance);
        regions = std::move(loadedRegions);
        if (headless)
        {
            expectedImportedChunks.store(chunksWithinBlasDistance, std::memory_order_relaxed);
            importedChunksEnqueuedForBlas.store(0, std::memory_order_relaxed);
            worldImportActive.store(true, std::memory_order_release);
        }
        Renderer::restoreCameraFromImport(cameraPosInt, cameraPosFloat, phi, theta);
        setDirty();
        Logger::log("world import: imported %u chunks across %zu regions from %s; expectedImported=%u",
                    totalChunks, regions.size(), worldDir.generic_string().c_str(), chunksWithinBlasDistance);
        return true;
    }
    catch (const std::exception& error)
    {
        Logger::logError("world import: %s: %s", worldDir.generic_string().c_str(), error.what());
        return false;
    }
}

void importWorld()
{
    const std::string worldPathStr = SettingsManager::getAsString("world");
    if (worldPathStr.empty())
    {
        return;
    }
    if (!importWorldImpl(worldPathStr))
    {
        exit(1);
    }
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

    chunksToGenerateTerrain.clear();
    chunksToGenerateGeometry.clear();
    {
        std::scoped_lock<std::mutex> lock(chunksToCreateBlasMutex);
        chunksToCreateBlas.clear();
    }
    {
        std::scoped_lock<std::mutex> lock(chunksToDestroyMutex);
        chunksToDestroy.clear();
    }
    {
        std::scoped_lock<std::mutex> lock(chunksToRevisitMutex);
        chunksToRevisit.clear();
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
    threadPool.shutdown();

    resetTerrainState();

    threadPool.init();

    if (!importWorldImpl(worldDir))
    {
        Logger::logError("world reimport: failed; terrain will regenerate from current settings");
    }
}

bool pollHeadlessImport()
{
    if (!worldImportActive.load(std::memory_order_relaxed))
    {
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
    TerrainOmm::reset();
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
