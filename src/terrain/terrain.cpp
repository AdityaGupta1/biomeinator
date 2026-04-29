// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
#include "settings_manager.h"
#include "logger.h"
#include "structure/structure.h"
#include "util/file_util.h"
#include "util/glm_util.h"
#include "util/rng.h"

#include <lz4.h>
#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#define DEBUG_SINGLE_THREAD 0


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

static uint32_t expectedBlasBuildChunks{ 0 };
static uint32_t completedBlasBuildChunks{ 0 };
static bool worldImportActive{ false };

inline constexpr uint32_t maxTasksPerFrame = 48;
inline constexpr uint32_t maxNumGenerateTerrainTasksPerFrame = 12;

void update(ToFreeList& toFreeList)
{
    const int renderDistance = SettingsManager::getAsInt("renderDistance");
    const int createBlasDistance = renderDistance + 1;
    const int fillStructuresDistance = createBlasDistance + 1;
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

void exportWorld()
{
    const std::filesystem::path exportsDir = FileUtil::getDocumentsDir("exports");
    if (exportsDir.empty())
    {
        Logger::logError("exportWorld: failed to get Documents directory");
        return;
    }

    const std::filesystem::path exportDir = exportsDir / FileUtil::getTimestampString();
    std::filesystem::create_directories(exportDir);

    const Camera& camera = Renderer::getCamera();
    const glm::ivec3 cameraPosInt = camera.getPosInt_WS();
    const glm::vec3 cameraPosFloat = camera.getPosFloat_WS();
    const float phi = camera.getPhi();
    const float theta = camera.getTheta();

    const int renderDistance = SettingsManager::getAsInt("renderDistance");
    const uint32_t worldSeed = SettingsManager::getWorldSeed();

    std::vector<glm::ivec2> regionPositions;
    uint32_t totalChunksExported = 0;
    uint32_t totalRegionsExported = 0;

    const int blockBiomePayloadSize = static_cast<int>(numChunkBlocks * sizeof(uint16_t) + chunkSizeXZSquare * sizeof(uint8_t));
    const int maxCompressedSize = LZ4_compressBound(blockBiomePayloadSize);
    std::vector<char> compressBuffer(maxCompressedSize);
    std::vector<char> blockBiomeBuffer(blockBiomePayloadSize);

    for (const auto& [regionPos, regionPtr] : regions)
    {
        if (!regionPtr)
        {
            continue;
        }

        const Region& region = *regionPtr;

        struct ChunkEntry
        {
            uint16_t localIdx;
            Chunk* chunk;
        };
        std::vector<ChunkEntry> populatedChunks;

        for (uint32_t i = 0; i < region.chunks.size(); ++i)
        {
            const std::unique_ptr<Chunk>& chunkPtr = region.chunks[i];
            if (!chunkPtr)
            {
                continue;
            }

            const ChunkState state = chunkPtr->getState();
            if (state >= ChunkState::HAS_ALL_BLOCKS)
            {
                populatedChunks.push_back({ static_cast<uint16_t>(i), chunkPtr.get() });
            }
        }

        if (populatedChunks.empty())
        {
            continue;
        }

        char regionFileName[64];
        sprintf_s(regionFileName, "region_%d_%d.bin", regionPos.x, regionPos.y);
        const std::filesystem::path regionFilePath = exportDir / regionFileName;

        std::ofstream file(regionFilePath, std::ios::binary);
        if (!file)
        {
            Logger::logError("exportWorld: failed to create %s", regionFilePath.generic_string().c_str());
            continue;
        }

        const uint32_t magic = 0x42494F4D;
        const uint16_t version = 1;
        const int32_t regionX = regionPos.x;
        const int32_t regionZ = regionPos.y;
        const uint16_t numPopulatedChunks = static_cast<uint16_t>(populatedChunks.size());

        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        file.write(reinterpret_cast<const char*>(&regionX), sizeof(regionX));
        file.write(reinterpret_cast<const char*>(&regionZ), sizeof(regionZ));
        file.write(reinterpret_cast<const char*>(&numPopulatedChunks), sizeof(numPopulatedChunks));

        for (const ChunkEntry& entry : populatedChunks)
        {
            const Chunk& chunk = *entry.chunk;
            const ChunkState state = chunk.getState();

            const bool hasBlocks = state >= ChunkState::HAS_ALL_BLOCKS;
            const bool hasSegments = state >= ChunkState::NEEDS_GEOMETRY;

            const uint8_t chunkStateValue = static_cast<uint8_t>(state);
            const uint8_t flags = (hasBlocks ? 0x01 : 0x00) | (hasSegments ? 0x02 : 0x00);

            uint32_t compressedBlocksSize = 0;
            if (hasBlocks)
            {
                const std::vector<Block>& blocks = chunk.getBlocks();
                const std::vector<Biome>& biomes = chunk.getBiomes();

                memcpy(blockBiomeBuffer.data(),
                       blocks.data(),
                       numChunkBlocks * sizeof(uint16_t));
                memcpy(blockBiomeBuffer.data() + numChunkBlocks * sizeof(uint16_t),
                       biomes.data(),
                       chunkSizeXZSquare * sizeof(uint8_t));

                const int compressed = LZ4_compress_default(
                    blockBiomeBuffer.data(),
                    compressBuffer.data(),
                    blockBiomePayloadSize,
                    maxCompressedSize);

                if (compressed <= 0)
                {
                    Logger::logError("exportWorld: LZ4 block compression failed for chunk idx %u in region (%d, %d)",
                                     entry.localIdx, regionPos.x, regionPos.y);
                    continue;
                }

                compressedBlocksSize = static_cast<uint32_t>(compressed);
            }

            uint32_t compressedSegmentsSize = 0;
            std::vector<char> compressedSegmentsData;
            if (hasSegments)
            {
                const std::vector<glm::uvec3>& segments = chunk.getSegments();
                const uint32_t numSegments = static_cast<uint32_t>(segments.size());
                const int segmentsPayloadSize = static_cast<int>(
                    sizeof(uint32_t) + numSegments * 3 * sizeof(uint32_t));

                std::vector<char> segmentsBuffer(segmentsPayloadSize);
                memcpy(segmentsBuffer.data(), &numSegments, sizeof(uint32_t));
                if (numSegments > 0)
                {
                    memcpy(segmentsBuffer.data() + sizeof(uint32_t),
                           segments.data(),
                           numSegments * sizeof(glm::uvec3));
                }

                const int segmentsCompressBound = LZ4_compressBound(segmentsPayloadSize);
                compressedSegmentsData.resize(segmentsCompressBound);

                const int compressed = LZ4_compress_default(
                    segmentsBuffer.data(),
                    compressedSegmentsData.data(),
                    segmentsPayloadSize,
                    segmentsCompressBound);

                if (compressed <= 0)
                {
                    Logger::logError("exportWorld: LZ4 segment compression failed for chunk idx %u in region (%d, %d)",
                                     entry.localIdx, regionPos.x, regionPos.y);
                    continue;
                }

                compressedSegmentsSize = static_cast<uint32_t>(compressed);
            }

            file.write(reinterpret_cast<const char*>(&entry.localIdx), sizeof(uint16_t));
            file.write(reinterpret_cast<const char*>(&chunkStateValue), sizeof(uint8_t));
            file.write(reinterpret_cast<const char*>(&flags), sizeof(uint8_t));
            file.write(reinterpret_cast<const char*>(&compressedBlocksSize), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&compressedSegmentsSize), sizeof(uint32_t));

            if (compressedBlocksSize > 0)
            {
                file.write(compressBuffer.data(), compressedBlocksSize);
            }
            if (compressedSegmentsSize > 0)
            {
                file.write(compressedSegmentsData.data(), compressedSegmentsSize);
            }

            ++totalChunksExported;
        }

        regionPositions.push_back(regionPos);
        ++totalRegionsExported;
    }

    nlohmann::json sceneJson;
    sceneJson["version"] = 1;
    sceneJson["camera"] = {
        { "posInt", { cameraPosInt.x, cameraPosInt.y, cameraPosInt.z } },
        { "posFloat", { cameraPosFloat.x, cameraPosFloat.y, cameraPosFloat.z } },
        { "phi", phi },
        { "theta", theta },
    };
    sceneJson["renderDistance"] = renderDistance;
    sceneJson["worldSeed"] = worldSeed;

    nlohmann::json regionsArray = nlohmann::json::array();
    for (const glm::ivec2& pos : regionPositions)
    {
        regionsArray.push_back({ pos.x, pos.y });
    }
    sceneJson["regions"] = regionsArray;

    const std::filesystem::path sceneJsonPath = exportDir / "scene.json";
    std::ofstream jsonFile(sceneJsonPath);
    if (!jsonFile)
    {
        Logger::logError("exportWorld: failed to create scene.json");
        return;
    }
    jsonFile << sceneJson.dump();

    Logger::log("exportWorld: exported %u chunks across %u regions to %s",
                totalChunksExported, totalRegionsExported,
                exportDir.generic_string().c_str());
}

void importWorld()
{
    Logger::log("importWorld() called");
}

bool isWorldFullyLoaded()
{
    if (!worldImportActive)
    {
        return true;
    }
    return completedBlasBuildChunks >= expectedBlasBuildChunks;
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
