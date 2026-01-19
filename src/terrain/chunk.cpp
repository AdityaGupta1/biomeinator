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

#include "chunk.h"

#include "block.h"
#include "noise.h"
#include "terrain.h"
#include "terrain_materials.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_structs.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/component_wise.hpp>

#include <DirectXMath.h>
#include <vector>

using namespace glm;
using namespace DirectX;

#define DEFAULT_TEX_NUM_BLOCKS_X 32
#define DEFAULT_TEX_NUM_BLOCKS_Y 32

Chunk::Chunk(ivec2 chunkPos, Region* region)
	: chunkPos(chunkPos), region(region)
{
    const glm::ivec2 thisRegionPosChunks = this->region->regionPosChunks;
    for (int dirIdx = 0; dirIdx < 4; ++dirIdx)
    {
        const NeighborDirection dir = static_cast<NeighborDirection>(dirIdx);
        const glm::ivec2 neighborChunkPos = this->chunkPos + neighborOffset(dir);

        Region* neighborRegion = this->region;
        const glm::ivec2 neighborChunkPos_region = neighborChunkPos - thisRegionPosChunks;
        if (glm::compMin(neighborChunkPos_region) < 0 || glm::compMax(neighborChunkPos_region) >= REGION_SIDE_LENGTH)
        {
            neighborRegion = neighborRegion->getNeighbor(dir);
        }

        if (neighborRegion != nullptr)
        {
            Chunk* neighborChunk = neighborRegion->getChunk(neighborChunkPos);
            if (neighborChunk != nullptr)
            {
                this->atomicNeighbors[static_cast<size_t>(dir)].store(neighborChunk, std::memory_order_release);
                neighborChunk->atomicNeighbors[static_cast<size_t>(oppositeNeighborDirection(dir))].store(
                    this, std::memory_order_release);

                if (neighborChunk->getState() >= ChunkState::HAS_BLOCKS)
                {
                    this->numNeighborsWithBlocks.fetch_add(1, std::memory_order_acq_rel);
                }

                // at this point, this chunk cannot have blocks, so we don't need to update neighborChunk->numNeighborsWithBlocks
            }
        }
    }
}

void Chunk::generateBlocks()
{
    const ivec2 chunkBlockPos_WS = chunkPos * 16;

    for (uint z = 0; z < CHUNK_SIZE_XZ; ++z)
    {
        for (uint x = 0; x < CHUNK_SIZE_XZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkBlockPos_WS + ivec2(x, z);
            const uint height = uint(64.f + 10.f * (sinf(blockPosXZ_WS.x * 0.1f) * cosf(blockPosXZ_WS.y * 0.1f)));

            for (uint y = 0; y < height; ++y)
            {
                const ivec3 blockPos_CS = ivec3(x, y, z);
                const ivec3 blockPos_WS = ivec3(blockPosXZ_WS.x, y, blockPosXZ_WS.y);
                this->blocks[blockPosToIdx(blockPos_CS)] = rand1(uvec3(blockPos_WS)) < 0.04f ? Block::LAMP : Block::STONE;
            }

            if (rand1(uvec2(blockPosXZ_WS)) < 0.02f && height < CHUNK_SIZE_Y)
            {
                this->blocks[blockPosToIdx(ivec3(x, height, z))] = Block::LAMP;
            }
        }
    }

    this->advanceState(ChunkState::HAS_BLOCKS);

    bool setTerrainDirty = false;

    for (std::atomic<Chunk*>& atomicNeighbor : this->atomicNeighbors)
    {
        Chunk* neighbor = atomicNeighbor.load(std::memory_order_acquire);
        if (neighbor == nullptr)
        {
            continue;
        }

        const uint neighborNumNeighborsWithBlocks =
            neighbor->numNeighborsWithBlocks.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (neighborNumNeighborsWithBlocks == 4)
        {
            neighbor->onNeighborsHaveBlocks();
            setTerrainDirty = true;
        }
    }

    if (this->numNeighborsWithBlocks.load(std::memory_order_acquire) == 4)
    {
        this->onNeighborsHaveBlocks();
        setTerrainDirty = true;
    }

    if (setTerrainDirty)
    {
        Terrain::setDirty();
    }
}

void Chunk::onNeighborsHaveBlocks()
{
    if (this->onNeighborsHaveBlocksOnceFlag.exchange(true, std::memory_order_acq_rel))
    {
        return; // this function has already been run
    }

    this->advanceState(ChunkState::NEIGHBORS_HAVE_BLOCKS);

    for (int i = 0; i < 4; ++i)
    {
        this->neighbors[i] = this->atomicNeighbors[i].load(std::memory_order_acquire);
    }

    // TODO: build segment array
}

static inline DirectX::XMFLOAT2 vec2ToDirectX(const glm::vec2& v)
{
    return { v.x, v.y };
}

static inline DirectX::XMFLOAT3 vec3ToDirectX(const glm::vec3& v)
{
    return { v.x, v.y, v.z };
}

// first four match NeighborDirection enum
static constexpr ivec3 faceOffsets[6] = {
    ivec3(1, 0, 0),  // +x
    ivec3(0, 0, 1),  // +z
    ivec3(-1, 0, 0), // -x
    ivec3(0, 0, -1), // -z
    ivec3(0, 1, 0),  // +y
    ivec3(0, -1, 0), // -y
};

bool Chunk::isBlockAir(ivec3 pos_CS, int faceIdx)
{
    if (pos_CS.y < 0 || pos_CS.y >= CHUNK_SIZE_Y)
    {
        return true;
    }

    Block block;

    if (min(pos_CS.x, pos_CS.z) < 0 || max(pos_CS.x, pos_CS.z) >= CHUNK_SIZE_XZ)
    {
        const Chunk* neighborChunk = this->neighbors[faceIdx]; // faceIdx 0-3 corresponds to NeighborDirection
        ASSERT(neighborChunk != nullptr); // neighborChunk should exist because this function is not called until state == NEIGHBORS_HAVE_BLOCKS
        const ivec3 pos_neighborCS = {
            (pos_CS.x + CHUNK_SIZE_XZ) % CHUNK_SIZE_XZ,
            pos_CS.y,
            (pos_CS.z + CHUNK_SIZE_XZ) % CHUNK_SIZE_XZ,
        };
        block = neighborChunk->blocks[Chunk::blockPosToIdx(uvec3(pos_neighborCS))];
    }
    else
    {
        block = blocks[Chunk::blockPosToIdx(uvec3(pos_CS))];
    }

    return block == Block::AIR;
}

static constexpr ivec3 allFaceVertPositions[24] = {
    ivec3(1, 1, 0), ivec3(1, 1, 1), ivec3(1, 0, 1), ivec3(1, 0, 0), // +x
    ivec3(1, 1, 1), ivec3(0, 1, 1), ivec3(0, 0, 1), ivec3(1, 0, 1), // +z
    ivec3(0, 1, 1), ivec3(0, 1, 0), ivec3(0, 0, 0), ivec3(0, 0, 1), // -x
    ivec3(0, 1, 0), ivec3(1, 1, 0), ivec3(1, 0, 0), ivec3(0, 0, 0), // -z
    ivec3(1, 1, 1), ivec3(1, 1, 0), ivec3(0, 1, 0), ivec3(0, 1, 1), // +y
    ivec3(0, 0, 1), ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(1, 0, 1), // -y
};

static constexpr uvec2 uvOffsets[4] = {
    uvec2(1, 0),
    uvec2(0, 0),
    uvec2(0, 1),
    uvec2(1, 1),
};
static constexpr vec2 uvMultiplier = 1.f / vec2(DEFAULT_TEX_NUM_BLOCKS_X, DEFAULT_TEX_NUM_BLOCKS_Y);

void Chunk::setInstance(Instance* instance)
{
    this->instance = instance;
    this->instance->setVisible(this->getIsInstanceVisible());
}

void Chunk::createInstance(Scene* scene)
{
    const ivec2 chunkBlockPos_WS = this->chunkPos * 16;
    const XMMATRIX transform = XMMatrixTranslation(
        static_cast<float>(chunkBlockPos_WS.x),
        0.f,
        static_cast<float>(chunkBlockPos_WS.y)
    );
    XMFLOAT3X4 instanceTransform;
    XMStoreFloat3x4(&instanceTransform, transform);

    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> emissiveTriangleIdxs;

    constexpr size_t numVertsToReserve = 1 << 15;
    verts.reserve(numVertsToReserve);
    indices.reserve(numVertsToReserve * 6 / 4);
    emissiveTriangleIdxs.reserve(512);

    for (uint z = 0; z < CHUNK_SIZE_XZ; ++z)
    {
        for (uint x = 0; x < CHUNK_SIZE_XZ; ++x)
        {
            const uint32_t baseIdx = blockPosToIdx(uvec3(x, 0, z));

            for (uint y = 0; y < CHUNK_SIZE_Y; ++y)
            {
                const uvec3 blockPos_CS(x, y, z);
                const Block block = blocks[baseIdx + y];
                if (block == Block::AIR)
                {
                    continue;
                }

                const BlockData& blockData = Blocks::getBlockData(block);

                for (uint faceIdx = 0; faceIdx < 6; ++faceIdx)
                {
                    const ivec3 neighborOffset = faceOffsets[faceIdx];
                    const ivec3 neighborPos_CS = ivec3(blockPos_CS) + neighborOffset;

                    if (!isBlockAir(neighborPos_CS, faceIdx))
                    {
                        continue;
                    }

                    const DirectX::XMFLOAT3 normal = vec3ToDirectX(vec3(neighborOffset));
                    const ivec3* thisFaceVertPositions = allFaceVertPositions + (faceIdx * 4);
                    const uint32_t baseVertIdx = static_cast<uint32_t>(verts.size());
                    for (uint i = 0; i < 4; ++i)
                    {
                        const vec3 vertPos_CS = vec3(ivec3(blockPos_CS) + thisFaceVertPositions[i]);
                        const vec2 uv = (vec2(blockData.texCoords + uvOffsets[i])) * uvMultiplier;
                        verts.emplace_back(
                            vec3ToDirectX(vertPos_CS),
                            normal,
                            vec2ToDirectX(uv)
                        );
                    }

                    const uint32_t triangleIdx = static_cast<uint32_t>(indices.size() / 3u);

                    indices.emplace_back(baseVertIdx + 0u);
                    indices.emplace_back(baseVertIdx + 1u);
                    indices.emplace_back(baseVertIdx + 2u);
                    indices.emplace_back(baseVertIdx + 0u);
                    indices.emplace_back(baseVertIdx + 2u);
                    indices.emplace_back(baseVertIdx + 3u);

                    if (blockData.emitsLight)
                    {
                        emissiveTriangleIdxs.emplace_back(triangleIdx);
                        emissiveTriangleIdxs.emplace_back(triangleIdx + 1u);
                    }
                }
            }
        }
    }

    instance->setGeometry(instanceTransform, std::move(verts), std::move(indices));
    instance->setMaterialIdx(TerrainMaterials::getDefaultMaterialIdx());

    instance->addAreaLights(emissiveTriangleIdxs);

    this->advanceState(ChunkState::HAS_GEOMETRY);
    if (this->getIsMarkedForDestruction())
    {
        Terrain::addChunkToDestroy(this);
    }
    else
    {
        Terrain::addChunkToCreateBlas(this);
    }
}

void Chunk::destroyInstance(ToFreeList& toFreeList)
{
    toFreeList.pushInstance(this->instance);
    this->instance = nullptr;
    this->setState(ChunkState::NEIGHBORS_HAVE_BLOCKS); // neighbors must have had blocks for this chunk to have an instance
    this->setIsMarkedForDestruction(false);
}

Instance* Chunk::getInstance() const
{
    return instance;
}

ChunkState Chunk::getState() const
{
    return this->state.load(std::memory_order_acquire);
}

void Chunk::setState(ChunkState newState)
{
    this->state.store(newState, std::memory_order_release);
}

void Chunk::advanceState(ChunkState newState)
{
    ChunkState state = this->getState();
    while (state < newState && !this->state.compare_exchange_weak(state, newState, std::memory_order_acq_rel))
    {}
}

bool Chunk::getIsMarkedForDestruction()
{
    return this->isMarkedForDestruction.load(std::memory_order_acquire);
}

void Chunk::setIsMarkedForDestruction(bool marked)
{
    this->isMarkedForDestruction.store(marked, std::memory_order_release);
}

bool Chunk::getIsInstanceVisible() const
{
    return this->isInstanceVisible;
}

void Chunk::setInstanceVisible(bool visible)
{
    this->isInstanceVisible = visible;
    if (this->instance != nullptr)
    {
        this->instance->setVisible(this->isInstanceVisible);
    }
}

glm::ivec2 Chunk::getChunkPos() const
{
    return this->chunkPos;
}

// y changes fastest, then x, then z
//
// for loops should be written like this:
// for (uint z = 0; z < CHUNK_SIZE_Z; ++z)
// {
//     for (uint x = 0; x < CHUNK_SIZE_X; ++x)
//     {
//         for (uint y = 0; y < CHUNK_SIZE_Y; ++y)
//         {
//             // do stuff here
uint32_t Chunk::blockPosToIdx(glm::uvec3 chunkBlockPos)
{
    return chunkBlockPos.y
		 + chunkBlockPos.x * CHUNK_SIZE_Y
		 + chunkBlockPos.z * (CHUNK_SIZE_XZ * CHUNK_SIZE_Y);
}

Region::Region(glm::ivec2 regionPos)
    : regionPos(regionPos), regionPosChunks(regionPos * REGION_SIDE_LENGTH)
{}

Chunk* Region::getChunk(glm::ivec2 chunkPos)
{
    return this->chunks[chunkPosToIdx(chunkPos - this->regionPosChunks)].get();
}

Chunk* Region::getOrCreateChunk(glm::ivec2 chunkPos)
{
    const uint32_t chunkIdx = chunkPosToIdx(chunkPos - this->regionPosChunks);
    if (this->chunks[chunkIdx] == nullptr)
    {
        this->chunks[chunkIdx] = std::make_unique<Chunk>(chunkPos, this);
    }
    return this->chunks[chunkIdx].get();
}

Region* Region::getNeighbor(NeighborDirection dir) const
{
    return this->neighbors[static_cast<size_t>(dir)];
}

void Region::setNeighbor(NeighborDirection dir, Region* neighborRegion)
{
    this->neighbors[static_cast<size_t>(dir)] = neighborRegion;
    neighborRegion->neighbors[static_cast<size_t>(oppositeNeighborDirection(dir))] = this;
}

// x changes fastest, then z
//
// for loops should be written like this:
// for (uint z = 0; z < REGION_SIDE_LENGTH; ++z)
// {
//     for (uint x = 0; x < REGION_SIDE_LENGTH; ++x)
//     {
//         // do stuff here
uint32_t Region::chunkPosToIdx(glm::ivec2 regionChunkPos)
{
    return regionChunkPos.x
         + regionChunkPos.y /*z*/ * REGION_SIDE_LENGTH;
}
