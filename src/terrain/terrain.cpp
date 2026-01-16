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

#include <unordered_map>

#define RENDER_DISTANCE 5

namespace Terrain
{

static Scene* scene;

static ThreadPool threadPool{};

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

static std::unordered_map<glm::ivec2, std::unique_ptr<Chunk>, IVec2Hash> chunks;

static std::deque<Chunk*> chunksToCreateInstance;

void queueChunkForInstanceCreation(Chunk* chunk)
{
    chunksToCreateInstance.push_back(chunk);
}

static glm::ivec2 lastChunkPos{ INT_MAX, INT_MAX };

void update(ToFreeList& toFreeList)
{
    const DirectX::XMFLOAT3 cameraPos_WS = Renderer::getCamera().getPos_WS();
    const glm::ivec2 currentChunkPos =
        glm::ivec2(glm::floor(glm::vec2(cameraPos_WS.x, cameraPos_WS.z) / static_cast<float>(CHUNK_SIZE_XZ)));

    if (currentChunkPos != lastChunkPos)
    {
        // TODO: replace with spiral
        for (int dx = -RENDER_DISTANCE; dx <= RENDER_DISTANCE; ++dx)
        {
            for (int dy = -RENDER_DISTANCE; dy <= RENDER_DISTANCE; ++dy)
            {
                const glm::ivec2 newChunkPos = currentChunkPos + glm::ivec2(dx, dy);
                if (chunks.find(newChunkPos) == chunks.end())
                {
                    std::unique_ptr<Chunk> newChunk = std::make_unique<Chunk>(newChunkPos);
                    newChunk->generateBlocks();
                    chunks[newChunkPos] = std::move(newChunk);
                }

                const glm::ivec2 oldChunkPos = lastChunkPos + glm::ivec2(dx, dy);
                if (std::max(abs(oldChunkPos.x - currentChunkPos.x), abs(oldChunkPos.y - currentChunkPos.y)) > RENDER_DISTANCE)
                {
                    const auto it = chunks.find(oldChunkPos);
                    if (it != chunks.end())
                    {
                        toFreeList.pushInstance(it->second->getInstance());
                        chunks.erase(it);
                    }
                }
            }
        }

        lastChunkPos = currentChunkPos;
    }

    while (!chunksToCreateInstance.empty())
    {
        Chunk* chunk = chunksToCreateInstance.front();

        Instance* instance = scene->requestNewInstance(toFreeList);
        chunk->createInstance(scene, instance);

        chunksToCreateInstance.pop_front();
    }
}

} // namespace Terrain
