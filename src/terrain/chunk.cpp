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

    for (uint z = 0; z < chunkSizeXZ; ++z)
    {
        for (uint x = 0; x < chunkSizeXZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkBlockPos_WS + ivec2(x, z);
            const uint height = uint(64.f + 10.f * (sinf(blockPosXZ_WS.x * 0.1f) * cosf(blockPosXZ_WS.y * 0.1f)));

            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(x, z));

            for (uint y = 0; y < height; ++y)
            {
                const ivec3 blockPos_CS = ivec3(x, y, z);
                const ivec3 blockPos_WS = ivec3(blockPosXZ_WS.x, y, blockPosXZ_WS.y);
                this->blocks[blockIdx++] = rand1(uvec3(blockPos_WS)) < 0.04f ? Block::LAMP : Block::STONE;
            }

            if (rand1(uvec2(blockPosXZ_WS)) < 0.02f && height < chunkSizeY)
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

    this->generateSegments();
}

// endPos is exclusive
bool Chunk::isSegmentAirOrSolid(const uvec3 startPos, const uvec3 endPos, bool isAirPredicate)
{
    for (uint blockZ = startPos.z; blockZ < endPos.z; ++blockZ)
    {
        for (uint blockX = startPos.x; blockX < endPos.x; ++blockX)
        {
            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(blockX, blockZ));

            for (uint blockY = startPos.y; blockY < endPos.y; ++blockY)
            {
                const Block block = this->blocks[blockIdx++];
                if ((block == Block::AIR) != isAirPredicate)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// endPos is exclusive
bool Chunk::isSegmentSurroundedBySolid(const uvec3 startPos, const uvec3 endPos, const uvec3 chunkSegmentPos) // TODO: reduce arithmetic in index calculations
{
    // these two cases should be skipped by generateSegments()
    ASSERT(chunkSegmentPos.y != 0);
    ASSERT(chunkSegmentPos.y != numChunkSegmentsY - 1);

    const uint thisSegmentIdx = Chunk::segmentPosToIdx(chunkSegmentPos);

    // -x
    {
        Block* blocks;
        uint blockX;
        bool check = true;
        if (chunkSegmentPos.x == 0)
        {
            Chunk* neighbor = this->neighbors[static_cast<uint8_t>(NeighborDirection::X_NEG)];
            ASSERT(neighbor != nullptr);
            blocks = neighbor->blocks.data();
            blockX = chunkSizeXZ - 1;
        }
        else
        {
            blocks = this->blocks.data();
            blockX = startPos.x - 1;
            check = (this->segments[thisSegmentIdx - numChunkSegmentsY] != ChunkSegment::BLOCKS_SURROUNDED);
        }

        if (check)
        {
            for (uint blockZ = startPos.z; blockZ < endPos.z; ++blockZ)
            {
                for (uint blockY = startPos.y; blockY < endPos.y; ++blockY)
                {
                    const Block block = blocks[Chunk::blockPosToIdx(uvec3(blockX, blockY, blockZ))];
                    if (block == Block::AIR)
                    {
                        return false;
                    }
                }
            }
        }
    }

    // -y
    if (this->segments[thisSegmentIdx - 1] != ChunkSegment::BLOCKS_SURROUNDED)
    {
        const uint blockY = startPos.y - 1;
        for (uint blockZ = startPos.z; blockZ < endPos.z; ++blockZ)
        {
            for (uint blockX = startPos.x; blockX < endPos.x; ++blockX)
            {
                const Block block = this->blocks[Chunk::blockPosToIdx(uvec3(blockX, blockY, blockZ))];
                if (block == Block::AIR)
                {
                    return false;
                }
            }
        }
    }

    // -z
    {
        Block* blocks;
        uint blockZ;
        bool check = true;
        if (chunkSegmentPos.z == 0)
        {
            Chunk* neighbor = this->neighbors[static_cast<uint8_t>(NeighborDirection::Z_NEG)];
            ASSERT(neighbor != nullptr);
            blocks = neighbor->blocks.data();
            blockZ = chunkSizeXZ - 1;
        }
        else
        {
            blocks = this->blocks.data();
            blockZ = startPos.z - 1;
            check = (this->segments[thisSegmentIdx - (numChunkSegmentsXZ * numChunkSegmentsY)] != ChunkSegment::BLOCKS_SURROUNDED);
        }

        if (check)
        {
            for (uint blockX = startPos.x; blockX < endPos.x; ++blockX)
            {
                for (uint blockY = startPos.y; blockY < endPos.y; ++blockY)
                {
                    const Block block = blocks[Chunk::blockPosToIdx(uvec3(blockX, blockY, blockZ))];
                    if (block == Block::AIR)
                    {
                        return false;
                    }
                }
            }
        }
    }

    { // +x
        Block* blocks;
        uint blockX;
        if (chunkSegmentPos.x == numChunkSegmentsXZ - 1)
        {
            Chunk* neighbor = this->neighbors[static_cast<uint8_t>(NeighborDirection::X_POS)];
            ASSERT(neighbor != nullptr);
            blocks = neighbor->blocks.data();
            blockX = 0;
        }
        else
        {
            blocks = this->blocks.data();
            blockX = endPos.x;
        }

        for (uint blockZ = startPos.z; blockZ < endPos.z; ++blockZ)
        {
            for (uint blockY = startPos.y; blockY < endPos.y; ++blockY)
            {
                const Block block = blocks[Chunk::blockPosToIdx(uvec3(blockX, blockY, blockZ))];
                if (block == Block::AIR)
                {
                    return false;
                }
            }
        }
    }

    // +y
    {
        const uint blockY = endPos.y;
        for (uint blockZ = startPos.z; blockZ < endPos.z; ++blockZ)
        {
            for (uint blockX = startPos.x; blockX < endPos.x; ++blockX)
            {
                const Block block = this->blocks[Chunk::blockPosToIdx(uvec3(blockX, blockY, blockZ))];
                if (block == Block::AIR)
                {
                    return false;
                }
            }
        }
    }

    { // +z
        Block* blocks;
        uint blockZ;
        if (chunkSegmentPos.z == numChunkSegmentsXZ - 1)
        {
            Chunk* neighbor = this->neighbors[static_cast<uint8_t>(NeighborDirection::Z_POS)];
            ASSERT(neighbor != nullptr);
            blocks = neighbor->blocks.data();
            blockZ = 0;
        }
        else
        {
            blocks = this->blocks.data();
            blockZ = endPos.z;
        }

        for (uint blockX = startPos.x; blockX < endPos.x; ++blockX)
        {
            for (uint blockY = startPos.y; blockY < endPos.y; ++blockY)
            {
                const Block block = blocks[Chunk::blockPosToIdx(uvec3(blockX, blockY, blockZ))];
                if (block == Block::AIR)
                {
                    return false;
                }
            }
        }
    }
}

void Chunk::generateSegments()
{
    uint32_t segmentIdx = 0;
    for (uint segmentZ = 0; segmentZ < numChunkSegmentsXZ; ++segmentZ)
    {
        for (uint segmentX = 0; segmentX < numChunkSegmentsXZ; ++segmentX)
        {
            uint segmentIdx = Chunk::segmentPosXZToIdx(uvec2(segmentX, segmentZ));

            for (uint segmentY = 0; segmentY < numChunkSegmentsY; ++segmentY)
            {
                const uvec3 segmentBasePos(
                    segmentX * chunkSegmentSizeXZ, segmentY * chunkSegmentSizeY, segmentZ * chunkSegmentSizeXZ);
                const uvec3 maxPos = segmentBasePos + uvec3(chunkSegmentSizeXZ, chunkSegmentSizeY, chunkSegmentSizeXZ);

                ChunkSegment segment = ChunkSegment::MIXED;

                const Block blockAtBasePos = this->blocks[Chunk::blockPosToIdx(segmentBasePos)];
                const bool isAirPredicate = blockAtBasePos == Block::AIR;
                if (!isAirPredicate && (segmentY == 0 || segmentY == numChunkSegmentsY - 1))
                {
                    // this segment has blocks and cannot be surrounded because it's at the top or bottom of the chunk
                    segment = ChunkSegment::MIXED;
                }
                else if (this->isSegmentAirOrSolid(segmentBasePos, maxPos, isAirPredicate))
                {
                    if (isAirPredicate)
                    {
                        segment = ChunkSegment::AIR;
                    }
                    else
                    {
                        const uvec3 chunkSegmentPos(segmentX, segmentY, segmentZ);
                        const bool isSurrounded = isSegmentSurroundedBySolid(segmentBasePos, maxPos, chunkSegmentPos);
                        segment = isSurrounded ? ChunkSegment::BLOCKS_SURROUNDED : ChunkSegment::MIXED;
                    }
                }

                this->segments[segmentIdx++] = segment;
            }
        }
    }
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
    if (pos_CS.y < 0 || pos_CS.y >= chunkSizeY)
    {
        return true;
    }

    Block block;

    if (min(pos_CS.x, pos_CS.z) < 0 || max(pos_CS.x, pos_CS.z) >= chunkSizeXZ)
    {
        const Chunk* neighborChunk = this->neighbors[faceIdx]; // faceIdx 0-3 corresponds to NeighborDirection
        ASSERT(neighborChunk != nullptr); // neighborChunk should exist because this function is not called until state == NEIGHBORS_HAVE_BLOCKS
        const ivec3 pos_neighborCS = {
            (pos_CS.x + chunkSizeXZ) % chunkSizeXZ,
            pos_CS.y,
            (pos_CS.z + chunkSizeXZ) % chunkSizeXZ,
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

    uint32_t blockIdx = 0;
    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            for (uint blockY = 0; blockY < chunkSizeY; ++blockY)
            {
                const uvec3 blockPos_CS(blockX, blockY, blockZ);
                const Block block = blocks[blockIdx++];
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
//         for (uint y = 0; y < chunkSizeY; ++y)
//         {
//             // do stuff here
uint32_t Chunk::blockPosToIdx(glm::uvec3 chunkBlockPos)
{
    return chunkBlockPos.y
		 + chunkBlockPos.x * chunkSizeY
		 + chunkBlockPos.z * (chunkSizeXZ * chunkSizeY);
}

uint32_t Chunk::blockPosXZToIdx(glm::uvec2 chunkBlockPos)
{
    return chunkBlockPos.x * chunkSizeY
         + chunkBlockPos.y /*z*/ * (chunkSizeXZ * chunkSizeY);
}

uint32_t Chunk::segmentPosToIdx(glm::uvec3 chunkSegmentPos)
{
    return chunkSegmentPos.y
         + chunkSegmentPos.x * numChunkSegmentsY
         + chunkSegmentPos.z * (numChunkSegmentsXZ * numChunkSegmentsY);
}

uint32_t Chunk::segmentPosXZToIdx(glm::uvec2 chunkSegmentPos)
{
    return chunkSegmentPos.x * numChunkSegmentsY
         + chunkSegmentPos.y /*z*/ * (numChunkSegmentsXZ * numChunkSegmentsY);
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
