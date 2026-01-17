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

#include "block.h"
#include "chunk.h"
#include "noise.h"
#include "terrain_materials.h"
#include "multithreading/thread_pool.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/camera.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/component_wise.hpp>

#define RENDER_DISTANCE 15

#define DEBUG_SINGLE_THREAD 1

namespace Terrain
{

static Scene* scene;

void init(Scene* scene)
{
    Terrain::scene = scene;

    TerrainMaterials::init(scene);
    Blocks::init();
}

struct IVec2Hash
{
    size_t operator()(const glm::ivec2& v) const noexcept
    {
        return combinedHash(static_cast<uint32_t>(v.x), static_cast<uint32_t>(v.y));
    }
};

static std::unordered_map<glm::ivec2, std::unique_ptr<Region>, IVec2Hash> regions;
static std::vector<Chunk*> chunksToCreateInstance;
static std::vector<Chunk*> chunksToCreateBlas;
static std::mutex chunksToCreateBlasMutex;
static std::vector<Chunk*> chunksToDestroy;
static std::mutex chunksToDestroyMutex;

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

static ThreadPool threadPool{};

static glm::ivec2 lastChunkPos{ INT_MAX, INT_MAX };

static constexpr glm::ivec2 regionNeighborOffsets[4] = {
    { 1, 0 },  // X_POS
    { 0, 1 },  // Z_POS
    { -1, 0 }, // X_NEG
    { 0, -1 }, // Z_NEG
};

void update(ToFreeList& toFreeList)
{
    const DirectX::XMFLOAT3 cameraPos_WS = Renderer::getCamera().getPos_WS();
    const glm::ivec2 currentChunkPos =
        glm::ivec2(glm::floor(glm::vec2(cameraPos_WS.x, cameraPos_WS.z) / static_cast<float>(CHUNK_SIZE_XZ)));

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
        const glm::ivec2 minChunkPos = glm::min(currentChunkPos, lastChunkPos) - RENDER_DISTANCE;
        const glm::ivec2 maxChunkPos = glm::max(currentChunkPos, lastChunkPos) + RENDER_DISTANCE;

        const glm::ivec2 minRegionPos =
            glm::ivec2(glm::floor(glm::vec2(minChunkPos) / static_cast<float>(REGION_SIDE_LENGTH)));
        const glm::ivec2 maxRegionPos =
            glm::ivec2(glm::floor(glm::vec2(maxChunkPos) / static_cast<float>(REGION_SIDE_LENGTH)));

        for (int regionZ = minRegionPos.y; regionZ <= maxRegionPos.y; ++regionZ)
        {
            for (int regionX = minRegionPos.x; regionX <= maxRegionPos.x; ++regionX)
            {
                const glm::ivec2 regionPos = glm::ivec2(regionX, regionZ);

                const auto [regionIter, inserted] = regions.try_emplace(regionPos, nullptr);
                if (inserted)
                {
                    regionIter->second = std::make_unique<Region>(regionPos);

                    for (int neighborDirIdx = 0; neighborDirIdx < 4; ++neighborDirIdx)
                    {
                        const glm::ivec2 neighborRegionPos = regionPos + regionNeighborOffsets[neighborDirIdx];
                        const auto neighborIter = regions.find(neighborRegionPos);
                        if (neighborIter != regions.end())
                        {
                            regionIter->second->setNeighbor(static_cast<NeighborDirection>(neighborDirIdx), neighborIter->second.get());
                        }
                    }
                }
                Region& region = *regionIter->second;

                const glm::ivec2 minChunkPosInRegion = glm::max(region.regionPosChunks, minChunkPos);
                const glm::ivec2 maxChunkPosInRegion = glm::min(region.regionPosChunks + REGION_SIDE_LENGTH - 1, maxChunkPos);

                for (int chunkZ = minChunkPosInRegion.y; chunkZ <= maxChunkPosInRegion.y; ++chunkZ)
                {
                    for (int chunkX = minChunkPosInRegion.x; chunkX <= maxChunkPosInRegion.x; ++chunkX)
                    {
                        const glm::ivec2 chunkPos = glm::ivec2(chunkX, chunkZ);

                        const bool inCurrentRenderDistance =
                            glm::compMax(glm::abs(chunkPos - currentChunkPos)) <= RENDER_DISTANCE;
                        const bool inLastRenderDistance =
                            glm::compMax(glm::abs(chunkPos - lastChunkPos)) <= RENDER_DISTANCE;
                        if (!inCurrentRenderDistance && !inLastRenderDistance)
                        {
                            continue;
                        }

                        Chunk* chunk = region.getOrCreateChunk(chunkPos);
                        const ChunkState chunkState = chunk->getState();

                        if (inCurrentRenderDistance)
                        {
                            chunk->setMarkedForDestruction(false);

                            if (chunkState == ChunkState::NEEDS_BLOCKS)
                            {
                                chunk->setState(ChunkState::GENERATING_BLOCKS);
#if DEBUG_SINGLE_THREAD == 0
                                threadPool.enqueue([chunk] {
                                    chunk->generateBlocks();
                                });
#else
                                chunk->generateBlocks();
#endif
                            }
                            else if (chunkState == ChunkState::HAS_BLOCKS)
                            {
                                chunksToCreateInstance.push_back(chunk);
                            }
                        }
                        else if (inLastRenderDistance)
                        {
                            if (chunkState == ChunkState::GENERATING_GEOMETRY)
                            {
                                chunk->setMarkedForDestruction();
                            }
                            else if (chunkState == ChunkState::HAS_GEOMETRY)
                            {
                                addChunkToDestroy(chunk);
                            }
                        }
                    }
                }
            }
        }

        for (Chunk* chunk : chunksToCreateInstance)
        {
            Instance* instance = scene->requestNewInstance(toFreeList);
            chunk->setState(ChunkState::GENERATING_GEOMETRY);
#if DEBUG_SINGLE_THREAD == 0
            threadPool.enqueue([chunk, instance] {
                chunk->createInstance(scene, instance);
            });
#else
            chunk->createInstance(scene, instance);
#endif
        }
        chunksToCreateInstance.clear();

        lastChunkPos = currentChunkPos;
    }

    std::vector<Chunk*> chunksToCreateBlasNow;
    {
        std::scoped_lock<std::mutex> lock(chunksToCreateBlasMutex);
        chunksToCreateBlasNow = std::move(chunksToCreateBlas);
        chunksToCreateBlas.clear();
    }
    for (Chunk* chunk : chunksToCreateBlasNow)
    {
        scene->markInstanceReadyForBlasBuild(chunk->getInstance());
    }

    std::vector<Chunk*> chunksToDestroyNow;
    {
        std::scoped_lock<std::mutex> lock(chunksToDestroyMutex);
        chunksToDestroyNow = std::move(chunksToDestroy);
        chunksToDestroy.clear();
    }
    for (Chunk* chunk : chunksToDestroyNow)
    {
        chunk->destroyInstance(toFreeList);
    }
}

} // namespace Terrain
