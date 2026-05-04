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
#include "rendering/renderer.h"
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

// Cached at Terrain::init. See knowledge/terrain/world_export_import.md (Cost containment).
static bool testMode{ false };

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
    Terrain::testMode = SettingsManager::isTestMode();
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

// Test-mode-only import-completion gate. See knowledge/terrain/world_export_import.md
// for timing/atomic-ordering rationale.
static std::atomic<uint32_t> expectedImportedChunks{ 0 };
static std::atomic<uint32_t> importedChunksEnqueuedForBlas{ 0 };
static std::atomic<bool> worldImportActive{ false };

void addChunkToCreateBlas(Chunk* chunk)
{
    std::scoped_lock<std::mutex> lock(chunksToCreateBlasMutex);
    chunksToCreateBlas.push_back(chunk);
    if (testMode && worldImportActive.load(std::memory_order_acquire) && chunk->getWasImported())
    {
        importedChunksEnqueuedForBlas.fetch_add(1, std::memory_order_relaxed);
    }
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
inline constexpr uint32_t maxNumGenerateTerrainTasksPerFrame = 12;

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

static constexpr uint32_t worldRegionMagic = 0x42494F4D;
static constexpr uint16_t worldRegionVersion = 5;
static constexpr uint32_t worldJsonVersion = 1;

static_assert(sizeof(Block) == sizeof(uint16_t), "World export format assumes 2-byte Block");
static_assert(sizeof(Biome) == sizeof(uint8_t), "World export format assumes 1-byte Biome");

static constexpr size_t blockBiomePayloadSize =
    numChunkBlocks * sizeof(Block) + chunkSizeXZSquare * sizeof(Biome);

// 4 bytes per Structure, packed into a uint32_t with the following bit layout
// (low to high):
//   bits  [0..7]   = type (8 bits)
//   bits  [8..11]  = localX (4 bits)
//   bits  [12..20] = y (9 bits)
//   bits  [21..24] = localZ (4 bits)
// Owner chunk origin is implicit from where the entry is stored, so only chunk-local
// position is serialized. No chunk should realistically have more than 512 structures
// in its 16x16xN footprint.
static constexpr size_t maxStructuresPerChunk = 512;
static constexpr size_t structureEntrySize = sizeof(uint32_t);
static constexpr size_t structuresScratchSize = sizeof(uint32_t) + maxStructuresPerChunk * structureEntrySize;

static_assert(chunkSizeXZ == 16, "Structure packing assumes 4-bit localX/localZ (chunkSizeXZ == 16)");
static_assert(chunkSizeY == 512, "Structure packing assumes 9-bit Y (chunkSizeY == 512)");
static_assert(static_cast<size_t>(StructureType::COUNT) <= 256, "Structure packing assumes 8-bit type");

static std::string regionFileName(glm::ivec2 regionPos)
{
    char buf[64];
    sprintf_s(buf, "region_%d_%d.bin", regionPos.x, regionPos.y);
    return buf;
}

void exportWorld()
{
    const std::filesystem::path exportsDir = FileUtil::getDocumentsDir("exports");
    if (exportsDir.empty())
    {
        Logger::logError("world export: failed to get Documents directory");
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

    const int maxCompressedSize = LZ4_compressBound(static_cast<int>(blockBiomePayloadSize));
    const int maxStructuresCompressedSize = LZ4_compressBound(static_cast<int>(structuresScratchSize));
    std::vector<char> blockBiomeBuffer(blockBiomePayloadSize);
    std::vector<char> structuresBuffer(structuresScratchSize);

    for (const auto& [regionPos, regionPtr] : regions)
    {
        if (!regionPtr)
        {
            continue;
        }

        const Region& region = *regionPtr;

        std::vector<std::pair<uint16_t, Chunk*>> populatedChunks;

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
                populatedChunks.emplace_back(static_cast<uint16_t>(i), chunkPtr.get());
            }
        }

        if (populatedChunks.empty())
        {
            continue;
        }

        std::vector<char> regionBuffer;

        const int32_t regionX = regionPos.x;
        const int32_t regionZ = regionPos.y;
        const uint16_t numPopulatedChunks = static_cast<uint16_t>(populatedChunks.size());

        const auto appendBytes = [&regionBuffer](const void* src, size_t bytes)
        {
            const char* p = static_cast<const char*>(src);
            regionBuffer.insert(regionBuffer.end(), p, p + bytes);
        };

        appendBytes(&worldRegionMagic, sizeof(worldRegionMagic));
        appendBytes(&worldRegionVersion, sizeof(worldRegionVersion));
        appendBytes(&regionX, sizeof(regionX));
        appendBytes(&regionZ, sizeof(regionZ));
        appendBytes(&numPopulatedChunks, sizeof(numPopulatedChunks));

        for (const auto& [localIdx, chunkPtr] : populatedChunks)
        {
            const Chunk& chunk = *chunkPtr;

            const std::vector<Block>& blocks = chunk.getBlocks();
            const std::vector<Biome>& biomes = chunk.getBiomes();

            memcpy(blockBiomeBuffer.data(),
                   blocks.data(),
                   numChunkBlocks * sizeof(Block));
            memcpy(blockBiomeBuffer.data() + numChunkBlocks * sizeof(Block),
                   biomes.data(),
                   chunkSizeXZSquare * sizeof(Biome));

            // reserve header slot; sizes patched in once known
            const size_t headerOffset = regionBuffer.size();
            regionBuffer.resize(headerOffset + sizeof(uint16_t) + 2 * sizeof(uint32_t));

            const size_t blocksOffset = regionBuffer.size();
            regionBuffer.resize(blocksOffset + maxCompressedSize);
            const int compressedBlocks = LZ4_compress_default(
                blockBiomeBuffer.data(),
                regionBuffer.data() + blocksOffset,
                static_cast<int>(blockBiomePayloadSize),
                maxCompressedSize);

            if (compressedBlocks <= 0)
            {
                Logger::logError("world export: LZ4 block compression failed for chunk idx %u in region (%d, %d); aborting export",
                                 localIdx, regionPos.x, regionPos.y);
                return;
            }

            regionBuffer.resize(blocksOffset + compressedBlocks);
            const uint32_t compressedBlocksSize = static_cast<uint32_t>(compressedBlocks);

            uint32_t compressedStructuresSize = 0;
            const std::vector<Structure>& structures = chunk.getStructures();
            if (!structures.empty())
            {
                const uint32_t numStructures = static_cast<uint32_t>(structures.size());
                ASSERT(numStructures <= maxStructuresPerChunk, "structure count exceeds max per chunk");
                const int structuresPayloadSize = static_cast<int>(
                    sizeof(uint32_t) + numStructures * structureEntrySize);

                const glm::ivec2 chunkOriginBlocksXZ_WS = chunk.getChunkPos() * static_cast<int>(chunkSizeXZ);

                memcpy(structuresBuffer.data(), &numStructures, sizeof(uint32_t));
                char* writePtr = structuresBuffer.data() + sizeof(uint32_t);
                for (const Structure& s : structures)
                {
                    const int32_t localX = s.pos_WS.x - chunkOriginBlocksXZ_WS.x;
                    const int32_t localZ = s.pos_WS.z - chunkOriginBlocksXZ_WS.y;
                    const int32_t y = s.pos_WS.y;
                    const uint32_t typeBits = static_cast<uint32_t>(s.type);

                    ASSERT(localX >= 0 && localX < static_cast<int32_t>(chunkSizeXZ), "structure localX out of range");
                    ASSERT(localZ >= 0 && localZ < static_cast<int32_t>(chunkSizeXZ), "structure localZ out of range");
                    ASSERT(y >= 0 && y < static_cast<int32_t>(chunkSizeY), "structure y out of range");
                    ASSERT(typeBits < 256, "structure type does not fit in 8 bits");

                    const uint32_t packed =
                        typeBits
                        | (static_cast<uint32_t>(localX) << 8)
                        | (static_cast<uint32_t>(y) << 12)
                        | (static_cast<uint32_t>(localZ) << 21);

                    memcpy(writePtr, &packed, sizeof(uint32_t));
                    writePtr += sizeof(uint32_t);
                }

                const size_t structuresOffset = regionBuffer.size();
                regionBuffer.resize(structuresOffset + maxStructuresCompressedSize);
                const int compressed = LZ4_compress_default(
                    structuresBuffer.data(),
                    regionBuffer.data() + structuresOffset,
                    structuresPayloadSize,
                    maxStructuresCompressedSize);

                if (compressed <= 0)
                {
                    Logger::logError("world export: LZ4 structure compression failed for chunk idx %u in region (%d, %d); aborting export",
                                     localIdx, regionPos.x, regionPos.y);
                    return;
                }

                regionBuffer.resize(structuresOffset + compressed);
                compressedStructuresSize = static_cast<uint32_t>(compressed);
            }

            char* headerPtr = regionBuffer.data() + headerOffset;
            memcpy(headerPtr, &localIdx, sizeof(uint16_t));
            memcpy(headerPtr + sizeof(uint16_t), &compressedBlocksSize, sizeof(uint32_t));
            memcpy(headerPtr + sizeof(uint16_t) + sizeof(uint32_t), &compressedStructuresSize, sizeof(uint32_t));

            ++totalChunksExported;
        }

        const std::filesystem::path regionFilePath = exportDir / regionFileName(regionPos);

        std::ofstream file(regionFilePath, std::ios::binary);
        if (!file)
        {
            Logger::logError("world export: failed to create %s; aborting export",
                             regionFilePath.generic_string().c_str());
            return;
        }
        file.write(regionBuffer.data(), regionBuffer.size());

        regionPositions.push_back(regionPos);
        ++totalRegionsExported;
    }

    nlohmann::json worldJson;
    worldJson["version"] = worldJsonVersion;
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
        Logger::logError("world export: failed to create world.json");
        return;
    }
    jsonFile << worldJson.dump();

    Logger::log("world export: exported %u chunks across %u regions to %s",
                totalChunksExported, totalRegionsExported,
                exportDir.generic_string().c_str());
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
    if (missingField)
    {
        return false;
    }

    const uint32_t fileVersion = outJson["version"].get<uint32_t>();
    if (fileVersion != worldJsonVersion)
    {
        Logger::logError("world import: unsupported world.json version %u (expected %u)",
                         fileVersion, worldJsonVersion);
        return false;
    }

    return true;
}

static bool loadRegionFile(const std::filesystem::path& regionFilePath,
                           const glm::ivec2& regionPos,
                           const glm::ivec2& cameraChunkPos,
                           int createBlasDistance,
                           Region& region,
                           std::vector<char>& blockBiomeBuffer,
                           std::vector<char>& structuresBuffer,
                           uint32_t& outChunksImported,
                           uint32_t& outChunksWithinBlasDistance)
{
    std::ifstream regionFile(regionFilePath, std::ios::binary);
    if (!regionFile)
    {
        Logger::logError("world import: failed to open %s",
                         regionFilePath.generic_string().c_str());
        return false;
    }

    const std::vector<char> fileBytes(
        (std::istreambuf_iterator<char>(regionFile)),
        std::istreambuf_iterator<char>());

    const char* readPtr = fileBytes.data();
    const char* const fileEnd = fileBytes.data() + fileBytes.size();

    const auto readBytes = [&](void* dest, size_t bytes) -> bool
    {
        if (readPtr + bytes > fileEnd)
        {
            Logger::logError("world import: unexpected EOF in %s",
                             regionFilePath.generic_string().c_str());
            return false;
        }
        memcpy(dest, readPtr, bytes);
        readPtr += bytes;
        return true;
    };

    uint32_t magic;
    uint16_t version;
    int32_t fileRegionX;
    int32_t fileRegionZ;
    uint16_t numPopulatedChunks;

    if (!readBytes(&magic, sizeof(magic)))
    {
        return false;
    }
    if (!readBytes(&version, sizeof(version)))
    {
        return false;
    }
    if (!readBytes(&fileRegionX, sizeof(fileRegionX)))
    {
        return false;
    }
    if (!readBytes(&fileRegionZ, sizeof(fileRegionZ)))
    {
        return false;
    }
    if (!readBytes(&numPopulatedChunks, sizeof(numPopulatedChunks)))
    {
        return false;
    }

    if (magic != worldRegionMagic)
    {
        Logger::logError("world import: bad magic 0x%08x in %s (expected 0x%08x)",
                         magic, regionFilePath.generic_string().c_str(), worldRegionMagic);
        return false;
    }
    if (version != worldRegionVersion)
    {
        Logger::logError("world import: unsupported version %u in %s (expected %u)",
                         version, regionFilePath.generic_string().c_str(), worldRegionVersion);
        return false;
    }
    if (fileRegionX != regionPos.x || fileRegionZ != regionPos.y)
    {
        Logger::logError("world import: region mismatch in %s (header says %d,%d)",
                         regionFilePath.generic_string().c_str(), fileRegionX, fileRegionZ);
        return false;
    }

    for (uint16_t i = 0; i < numPopulatedChunks; ++i)
    {
        uint16_t localIdx;
        uint32_t compressedBlocksSize;
        uint32_t compressedStructuresSize;

        if (!readBytes(&localIdx, sizeof(localIdx)))
        {
            return false;
        }
        if (!readBytes(&compressedBlocksSize, sizeof(compressedBlocksSize)))
        {
            return false;
        }
        if (!readBytes(&compressedStructuresSize, sizeof(compressedStructuresSize)))
        {
            return false;
        }

        if (readPtr + compressedBlocksSize > fileEnd)
        {
            Logger::logError("world import: blocks payload runs past EOF in %s",
                             regionFilePath.generic_string().c_str());
            return false;
        }
        const int decompressedBlocks = LZ4_decompress_safe(
            readPtr,
            blockBiomeBuffer.data(),
            static_cast<int>(compressedBlocksSize),
            static_cast<int>(blockBiomePayloadSize));
        readPtr += compressedBlocksSize;

        if (decompressedBlocks != static_cast<int>(blockBiomePayloadSize))
        {
            Logger::logError("world import: LZ4 block decompression failed for chunk idx %u in %s (got %d, expected %zu)",
                             localIdx, regionFilePath.generic_string().c_str(),
                             decompressedBlocks, blockBiomePayloadSize);
            return false;
        }

        std::vector<Block> blocks(numChunkBlocks);
        std::vector<Biome> biomes(chunkSizeXZSquare);
        memcpy(blocks.data(), blockBiomeBuffer.data(), numChunkBlocks * sizeof(Block));
        memcpy(biomes.data(),
               blockBiomeBuffer.data() + numChunkBlocks * sizeof(Block),
               chunkSizeXZSquare * sizeof(Biome));

        const int32_t chunkLocalX = localIdx % static_cast<int32_t>(regionSideLength);
        const int32_t chunkLocalZ = localIdx / static_cast<int32_t>(regionSideLength);
        const glm::ivec2 chunkPos = region.regionPosChunks + glm::ivec2(chunkLocalX, chunkLocalZ);
        const glm::ivec2 chunkOriginBlocksXZ_WS = chunkPos * static_cast<int>(chunkSizeXZ);

        std::vector<Structure> structures;
        if (compressedStructuresSize > 0)
        {
            if (readPtr + compressedStructuresSize > fileEnd)
            {
                Logger::logError("world import: structures payload runs past EOF in %s",
                                 regionFilePath.generic_string().c_str());
                return false;
            }
            const int decompressedStructures = LZ4_decompress_safe(
                readPtr,
                structuresBuffer.data(),
                static_cast<int>(compressedStructuresSize),
                static_cast<int>(structuresScratchSize));
            readPtr += compressedStructuresSize;

            if (decompressedStructures <= 0)
            {
                Logger::logError("world import: LZ4 structure decompression failed for chunk idx %u in %s (got %d; scratch=%zu)",
                                 localIdx, regionFilePath.generic_string().c_str(),
                                 decompressedStructures, structuresScratchSize);
                return false;
            }

            uint32_t numStructures;
            memcpy(&numStructures, structuresBuffer.data(), sizeof(uint32_t));

            const uint32_t expectedSize = sizeof(uint32_t) + numStructures * structureEntrySize;
            if (static_cast<uint32_t>(decompressedStructures) != expectedSize)
            {
                Logger::logError("world import: structures payload size mismatch for chunk idx %u in %s (got %d, expected %u)",
                                 localIdx, regionFilePath.generic_string().c_str(),
                                 decompressedStructures, expectedSize);
                return false;
            }

            structures.resize(numStructures);
            const char* structPtr = structuresBuffer.data() + sizeof(uint32_t);
            for (uint32_t s = 0; s < numStructures; ++s)
            {
                uint32_t packed;
                memcpy(&packed, structPtr, sizeof(uint32_t));
                structPtr += sizeof(uint32_t);

                const uint32_t typeBits = packed & 0xFFu;
                const int32_t localX = static_cast<int32_t>((packed >> 8) & 0xFu);
                const int32_t y = static_cast<int32_t>((packed >> 12) & 0x1FFu);
                const int32_t localZ = static_cast<int32_t>((packed >> 21) & 0xFu);

                structures[s].type = static_cast<StructureType>(typeBits);
                structures[s].pos_WS = glm::ivec3(
                    chunkOriginBlocksXZ_WS.x + localX,
                    y,
                    chunkOriginBlocksXZ_WS.y + localZ);
            }
        }

        Chunk* chunk = region.createChunk(chunkPos);
        chunk->loadSerializedData(std::move(blocks), std::move(biomes), std::move(structures));

        ++outChunksImported;
        if (glmUtil::chebyshevDistance(chunkPos, cameraChunkPos) <= createBlasDistance)
        {
            ++outChunksWithinBlasDistance;
        }
    }

    return true;
}

static bool importWorldImpl(const std::filesystem::path& worldDir)
{
    nlohmann::json worldJson;
    if (!loadAndValidateWorldJson(worldDir / "world.json", worldJson))
    {
        return false;
    }

    const uint32_t worldSeed = worldJson["worldSeed"].get<uint32_t>();
    SettingsManager::setWorldSeed(worldSeed);

    // ChunkGenerator caches worldSeed and noiseOffsetXZ at init time. Terrain::init
    // already ran once with whatever seed was active at startup, so re-init now that
    // the imported seed is in place — boundary chunks generated by the normal task
    // pipeline must use the same seed/offset that produced the exported chunks.
    ChunkGenerator::init();

    if (SettingsManager::isTestMode())
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

    const glm::ivec2 cameraChunkPos = glmUtil::floorDiv(
        glm::ivec2(cameraPosInt.x, cameraPosInt.z),
        glm::ivec2(static_cast<int>(chunkSizeXZ)));
    const int createBlasDistance = SettingsManager::getAsInt("renderDistance") + 1;

    std::vector<char> blockBiomeBuffer(blockBiomePayloadSize);
    std::vector<char> structuresBuffer(structuresScratchSize);

    uint32_t totalChunksImported = 0;
    uint32_t chunksWithinBlasDistance = 0;

    for (const nlohmann::json& regionEntry : worldJson["regions"])
    {
        const int32_t regionX = regionEntry[0].get<int32_t>();
        const int32_t regionZ = regionEntry[1].get<int32_t>();
        const glm::ivec2 regionPos{ regionX, regionZ };
        const std::filesystem::path regionFilePath = worldDir / regionFileName(regionPos);

        const auto [regionIter, inserted] = regions.try_emplace(regionPos, std::make_unique<Region>(regionPos));
        ASSERT(inserted, "region already exists at imported pos");
        Region& region = *regionIter->second;

        if (!loadRegionFile(regionFilePath, regionPos, cameraChunkPos, createBlasDistance,
                            region, blockBiomeBuffer, structuresBuffer,
                            totalChunksImported, chunksWithinBlasDistance))
        {
            return false;
        }
    }

    if (testMode)
    {
        expectedImportedChunks.store(chunksWithinBlasDistance, std::memory_order_relaxed);
        importedChunksEnqueuedForBlas.store(0, std::memory_order_relaxed);
        worldImportActive.store(true, std::memory_order_release);
    }

    Renderer::restoreCameraFromImport(cameraPosInt, cameraPosFloat, phi, theta);
    setDirty();

    Logger::log("world import: imported %u chunks across %zu regions from %s; expectedImported=%u",
                totalChunksImported, regions.size(),
                worldDir.generic_string().c_str(), chunksWithinBlasDistance);

    return true;
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
    tasksToEnqueue.clear();
    thisFrameTasks.clear();
    lastChunkPos = { INT_MAX, INT_MAX };
    cameraUnderwater = false;
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

bool pollTestModeImport()
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
