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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <deque>
#include <iterator>
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

    static_assert(sizeof(Block) == sizeof(uint16_t), "Export format assumes 2-byte Block");
    static_assert(sizeof(Biome) == sizeof(uint8_t), "Export format assumes 1-byte Biome");

    const int blockBiomePayloadSize = static_cast<int>(numChunkBlocks * sizeof(Block) + chunkSizeXZSquare * sizeof(Biome));
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

        std::vector<char> regionBuffer;

        const uint32_t magic = 0x42494F4D;
        const uint16_t version = 4;
        const int32_t regionX = regionPos.x;
        const int32_t regionZ = regionPos.y;
        const uint16_t numPopulatedChunks = static_cast<uint16_t>(populatedChunks.size());

        const auto appendBytes = [&regionBuffer](const void* src, size_t bytes)
        {
            const char* p = static_cast<const char*>(src);
            regionBuffer.insert(regionBuffer.end(), p, p + bytes);
        };

        appendBytes(&magic, sizeof(magic));
        appendBytes(&version, sizeof(version));
        appendBytes(&regionX, sizeof(regionX));
        appendBytes(&regionZ, sizeof(regionZ));
        appendBytes(&numPopulatedChunks, sizeof(numPopulatedChunks));

        for (const ChunkEntry& entry : populatedChunks)
        {
            const Chunk& chunk = *entry.chunk;

            const std::vector<Block>& blocks = chunk.getBlocks();
            const std::vector<Biome>& biomes = chunk.getBiomes();

            memcpy(blockBiomeBuffer.data(),
                   blocks.data(),
                   numChunkBlocks * sizeof(Block));
            memcpy(blockBiomeBuffer.data() + numChunkBlocks * sizeof(Block),
                   biomes.data(),
                   chunkSizeXZSquare * sizeof(Biome));

            const int compressedBlocks = LZ4_compress_default(
                blockBiomeBuffer.data(),
                compressBuffer.data(),
                blockBiomePayloadSize,
                maxCompressedSize);

            if (compressedBlocks <= 0)
            {
                Logger::logError("exportWorld: LZ4 block compression failed for chunk idx %u in region (%d, %d); aborting export",
                                 entry.localIdx, regionPos.x, regionPos.y);
                return;
            }

            const uint32_t compressedBlocksSize = static_cast<uint32_t>(compressedBlocks);

            uint32_t compressedStructuresSize = 0;
            std::vector<char> compressedStructuresData;
            const std::vector<Structure>& structures = chunk.getStructures();
            if (!structures.empty())
            {
                const uint32_t numStructures = static_cast<uint32_t>(structures.size());
                constexpr uint32_t structureEntrySize = sizeof(uint8_t) + 3 * sizeof(int32_t);
                const int structuresPayloadSize = static_cast<int>(
                    sizeof(uint32_t) + numStructures * structureEntrySize);

                std::vector<char> structuresBuffer(structuresPayloadSize);
                memcpy(structuresBuffer.data(), &numStructures, sizeof(uint32_t));
                char* writePtr = structuresBuffer.data() + sizeof(uint32_t);
                for (const Structure& s : structures)
                {
                    const uint8_t typeByte = static_cast<uint8_t>(s.type);
                    const int32_t posArr[3] = { s.pos_WS.x, s.pos_WS.y, s.pos_WS.z };
                    memcpy(writePtr, &typeByte, sizeof(uint8_t));
                    writePtr += sizeof(uint8_t);
                    memcpy(writePtr, posArr, sizeof(posArr));
                    writePtr += sizeof(posArr);
                }

                const int structuresCompressBound = LZ4_compressBound(structuresPayloadSize);
                compressedStructuresData.resize(structuresCompressBound);

                const int compressed = LZ4_compress_default(
                    structuresBuffer.data(),
                    compressedStructuresData.data(),
                    structuresPayloadSize,
                    structuresCompressBound);

                if (compressed <= 0)
                {
                    Logger::logError("exportWorld: LZ4 structure compression failed for chunk idx %u in region (%d, %d); aborting export",
                                     entry.localIdx, regionPos.x, regionPos.y);
                    return;
                }

                compressedStructuresSize = static_cast<uint32_t>(compressed);
            }

            appendBytes(&entry.localIdx, sizeof(uint16_t));
            appendBytes(&compressedBlocksSize, sizeof(uint32_t));
            appendBytes(&compressedStructuresSize, sizeof(uint32_t));

            appendBytes(compressBuffer.data(), compressedBlocksSize);
            if (compressedStructuresSize > 0)
            {
                appendBytes(compressedStructuresData.data(), compressedStructuresSize);
            }

            ++totalChunksExported;
        }

        char regionFileName[64];
        sprintf_s(regionFileName, "region_%d_%d.bin", regionPos.x, regionPos.y);
        const std::filesystem::path regionFilePath = exportDir / regionFileName;

        std::ofstream file(regionFilePath, std::ios::binary);
        if (!file)
        {
            Logger::logError("exportWorld: failed to create %s; aborting export",
                             regionFilePath.generic_string().c_str());
            return;
        }
        file.write(regionBuffer.data(), regionBuffer.size());

        regionPositions.push_back(regionPos);
        ++totalRegionsExported;
    }

    nlohmann::json worldJson;
    worldJson["version"] = 1;
    worldJson["camera"] = {
        { "posInt", { cameraPosInt.x, cameraPosInt.y, cameraPosInt.z } },
        { "posFloat", { cameraPosFloat.x, cameraPosFloat.y, cameraPosFloat.z } },
        { "phi", phi },
        { "theta", theta },
    };
    worldJson["renderDistance"] = renderDistance;
    worldJson["worldSeed"] = worldSeed;

    nlohmann::json regionsArray = nlohmann::json::array();
    for (const glm::ivec2& pos : regionPositions)
    {
        regionsArray.push_back({ pos.x, pos.y });
    }
    worldJson["regions"] = regionsArray;

    const std::filesystem::path worldJsonPath = exportDir / "world.json";
    std::ofstream jsonFile(worldJsonPath);
    if (!jsonFile)
    {
        Logger::logError("exportWorld: failed to create world.json");
        return;
    }
    jsonFile << worldJson.dump();

    Logger::log("exportWorld: exported %u chunks across %u regions to %s",
                totalChunksExported, totalRegionsExported,
                exportDir.generic_string().c_str());
}

void importWorld()
{
    const std::string worldPathStr = SettingsManager::getAsString("world");
    if (worldPathStr.empty())
    {
        return;
    }

    const std::filesystem::path worldDir = worldPathStr;
    const std::filesystem::path worldJsonPath = worldDir / "world.json";

    if (!std::filesystem::exists(worldJsonPath))
    {
        Logger::logError("importWorld: world.json not found at %s",
                         worldJsonPath.generic_string().c_str());
        exit(1);
    }

    nlohmann::json worldJson;
    {
        std::ifstream jsonFile(worldJsonPath);
        if (!jsonFile)
        {
            Logger::logError("importWorld: failed to open %s",
                             worldJsonPath.generic_string().c_str());
            exit(1);
        }

        try
        {
            worldJson = nlohmann::json::parse(jsonFile);
        }
        catch (const nlohmann::json::parse_error& e)
        {
            Logger::logError("importWorld: failed to parse %s: %s",
                             worldJsonPath.generic_string().c_str(), e.what());
            exit(1);
        }
    }

    const auto requireField = [&](const char* name)
    {
        if (!worldJson.contains(name))
        {
            Logger::logError("importWorld: world.json missing required field '%s'", name);
            exit(1);
        }
    };
    requireField("version");
    requireField("camera");
    requireField("renderDistance");
    requireField("worldSeed");
    requireField("regions");

    const uint32_t worldSeed = worldJson["worldSeed"].get<uint32_t>();
    SettingsManager::setWorldSeed(worldSeed);

    // ChunkGenerator caches worldSeed and noiseOffsetXZ at init time. Terrain::init
    // already ran once with whatever seed was active at startup, so re-init now that
    // the imported seed is in place — boundary chunks generated by the normal task
    // pipeline must use the same seed/offset that produced the exported chunks.
    ChunkGenerator::init();

    const bool testMode = !SettingsManager::getAsString("testOutput").empty();
    if (testMode)
    {
        SettingsManager::setAsInt("renderDistance", worldJson["renderDistance"].get<int>());
    }

    const nlohmann::json& cameraJson = worldJson["camera"];
    const glm::ivec3 cameraPosInt{
        cameraJson["posInt"][0].get<int>(),
        cameraJson["posInt"][1].get<int>(),
        cameraJson["posInt"][2].get<int>(),
    };
    const glm::vec3 cameraPosFloat{
        cameraJson["posFloat"][0].get<float>(),
        cameraJson["posFloat"][1].get<float>(),
        cameraJson["posFloat"][2].get<float>(),
    };
    const float phi = cameraJson["phi"].get<float>();
    const float theta = cameraJson["theta"].get<float>();

    static_assert(sizeof(Block) == sizeof(uint16_t), "Import format assumes 2-byte Block");
    static_assert(sizeof(Biome) == sizeof(uint8_t), "Import format assumes 1-byte Biome");

    constexpr size_t blockBiomePayloadSize =
        numChunkBlocks * sizeof(Block) + chunkSizeXZSquare * sizeof(Biome);
    constexpr size_t structuresScratchSize = 64 * 1024;

    std::vector<char> blockBiomeBuffer(blockBiomePayloadSize);
    std::vector<char> structuresBuffer(structuresScratchSize);

    uint32_t totalChunksImported = 0;

    for (const nlohmann::json& regionEntry : worldJson["regions"])
    {
        const int32_t regionX = regionEntry[0].get<int32_t>();
        const int32_t regionZ = regionEntry[1].get<int32_t>();
        const glm::ivec2 regionPos{ regionX, regionZ };

        char regionFileName[64];
        sprintf_s(regionFileName, "region_%d_%d.bin", regionX, regionZ);
        const std::filesystem::path regionFilePath = worldDir / regionFileName;

        std::ifstream regionFile(regionFilePath, std::ios::binary);
        if (!regionFile)
        {
            Logger::logError("importWorld: failed to open %s",
                             regionFilePath.generic_string().c_str());
            exit(1);
        }

        const std::vector<char> fileBytes(
            (std::istreambuf_iterator<char>(regionFile)),
            std::istreambuf_iterator<char>());

        const char* readPtr = fileBytes.data();
        const char* const fileEnd = fileBytes.data() + fileBytes.size();

        const auto readBytes = [&](void* dest, size_t bytes)
        {
            if (readPtr + bytes > fileEnd)
            {
                Logger::logError("importWorld: unexpected EOF in %s",
                                 regionFilePath.generic_string().c_str());
                exit(1);
            }
            memcpy(dest, readPtr, bytes);
            readPtr += bytes;
        };

        uint32_t magic;
        uint16_t version;
        int32_t fileRegionX;
        int32_t fileRegionZ;
        uint16_t numPopulatedChunks;

        readBytes(&magic, sizeof(magic));
        readBytes(&version, sizeof(version));
        readBytes(&fileRegionX, sizeof(fileRegionX));
        readBytes(&fileRegionZ, sizeof(fileRegionZ));
        readBytes(&numPopulatedChunks, sizeof(numPopulatedChunks));

        if (magic != 0x42494F4D)
        {
            Logger::logError("importWorld: bad magic 0x%08x in %s (expected 0x42494F4D)",
                             magic, regionFilePath.generic_string().c_str());
            exit(1);
        }
        if (version != 4)
        {
            Logger::logError("importWorld: unsupported version %u in %s (expected 4)",
                             version, regionFilePath.generic_string().c_str());
            exit(1);
        }
        if (fileRegionX != regionX || fileRegionZ != regionZ)
        {
            Logger::logError("importWorld: region mismatch in %s (header says %d,%d)",
                             regionFilePath.generic_string().c_str(), fileRegionX, fileRegionZ);
            exit(1);
        }

        const auto [regionIter, inserted] = regions.try_emplace(regionPos, nullptr);
        ASSERT(inserted);
        regionIter->second = std::make_unique<Region>(regionPos);
        Region& region = *regionIter->second;

        for (uint16_t i = 0; i < numPopulatedChunks; ++i)
        {
            uint16_t localIdx;
            uint32_t compressedBlocksSize;
            uint32_t compressedStructuresSize;

            readBytes(&localIdx, sizeof(localIdx));
            readBytes(&compressedBlocksSize, sizeof(compressedBlocksSize));
            readBytes(&compressedStructuresSize, sizeof(compressedStructuresSize));

            if (readPtr + compressedBlocksSize > fileEnd)
            {
                Logger::logError("importWorld: blocks payload runs past EOF in %s",
                                 regionFilePath.generic_string().c_str());
                exit(1);
            }
            const int decompressedBlocks = LZ4_decompress_safe(
                readPtr,
                blockBiomeBuffer.data(),
                static_cast<int>(compressedBlocksSize),
                static_cast<int>(blockBiomePayloadSize));
            readPtr += compressedBlocksSize;

            if (decompressedBlocks != static_cast<int>(blockBiomePayloadSize))
            {
                Logger::logError("importWorld: LZ4 block decompression failed for chunk idx %u in %s (got %d, expected %zu)",
                                 localIdx, regionFilePath.generic_string().c_str(),
                                 decompressedBlocks, blockBiomePayloadSize);
                exit(1);
            }

            std::vector<Block> blocks(numChunkBlocks);
            std::vector<Biome> biomes(chunkSizeXZSquare);
            memcpy(blocks.data(), blockBiomeBuffer.data(), numChunkBlocks * sizeof(Block));
            memcpy(biomes.data(),
                   blockBiomeBuffer.data() + numChunkBlocks * sizeof(Block),
                   chunkSizeXZSquare * sizeof(Biome));

            std::vector<Structure> structures;
            if (compressedStructuresSize > 0)
            {
                if (readPtr + compressedStructuresSize > fileEnd)
                {
                    Logger::logError("importWorld: structures payload runs past EOF in %s",
                                     regionFilePath.generic_string().c_str());
                    exit(1);
                }
                const int decompressedStructures = LZ4_decompress_safe(
                    readPtr,
                    structuresBuffer.data(),
                    static_cast<int>(compressedStructuresSize),
                    static_cast<int>(structuresScratchSize));
                readPtr += compressedStructuresSize;

                if (decompressedStructures <= 0)
                {
                    Logger::logError("importWorld: LZ4 structure decompression failed for chunk idx %u in %s (got %d; scratch=%zu)",
                                     localIdx, regionFilePath.generic_string().c_str(),
                                     decompressedStructures, structuresScratchSize);
                    exit(1);
                }

                uint32_t numStructures;
                memcpy(&numStructures, structuresBuffer.data(), sizeof(uint32_t));

                constexpr uint32_t structureEntrySize = sizeof(uint8_t) + 3 * sizeof(int32_t);
                const uint32_t expectedSize = sizeof(uint32_t) + numStructures * structureEntrySize;
                if (static_cast<uint32_t>(decompressedStructures) != expectedSize)
                {
                    Logger::logError("importWorld: structures payload size mismatch for chunk idx %u in %s (got %d, expected %u)",
                                     localIdx, regionFilePath.generic_string().c_str(),
                                     decompressedStructures, expectedSize);
                    exit(1);
                }

                structures.resize(numStructures);
                const char* structPtr = structuresBuffer.data() + sizeof(uint32_t);
                for (uint32_t s = 0; s < numStructures; ++s)
                {
                    uint8_t typeByte;
                    int32_t posArr[3];
                    memcpy(&typeByte, structPtr, sizeof(uint8_t));
                    structPtr += sizeof(uint8_t);
                    memcpy(posArr, structPtr, sizeof(posArr));
                    structPtr += sizeof(posArr);

                    structures[s].type = static_cast<StructureType>(typeByte);
                    structures[s].pos_WS = glm::ivec3(posArr[0], posArr[1], posArr[2]);
                }
            }

            const int32_t localX = localIdx % static_cast<int32_t>(regionSideLength);
            const int32_t localZ = localIdx / static_cast<int32_t>(regionSideLength);
            const glm::ivec2 chunkPos = region.regionPosChunks + glm::ivec2(localX, localZ);

            Chunk* chunk = region.createChunk(chunkPos);
            chunk->loadSerializedData(std::move(blocks), std::move(biomes), std::move(structures));

            ++totalChunksImported;
        }
    }

    const glm::ivec2 cameraChunkPos{
        MathUtil::floorDiv(cameraPosInt.x, static_cast<int>(chunkSizeXZ)),
        MathUtil::floorDiv(cameraPosInt.z, static_cast<int>(chunkSizeXZ)),
    };
    const int createBlasDistance = SettingsManager::getAsInt("renderDistance") + 1;

    uint32_t expected = 0;
    for (const auto& [regionPos, regionPtr] : regions)
    {
        if (!regionPtr)
        {
            continue;
        }

        for (const std::unique_ptr<Chunk>& chunkPtr : regionPtr->chunks)
        {
            if (!chunkPtr)
            {
                continue;
            }
            if (glmUtil::chebyshevDistance(chunkPtr->getChunkPos(), cameraChunkPos) <= createBlasDistance)
            {
                ++expected;
            }
        }
    }
    expectedBlasBuildChunks = expected;
    completedBlasBuildChunks = 0;
    worldImportActive = true;

    Renderer::restoreCamera(cameraPosInt, cameraPosFloat, phi, theta);
    setDirty();

    Logger::log("importWorld: imported %u chunks across %zu regions from %s; expectedBlas=%u",
                totalChunksImported, regions.size(),
                worldDir.generic_string().c_str(), expectedBlasBuildChunks);
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
