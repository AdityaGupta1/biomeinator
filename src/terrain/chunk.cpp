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
#include "chunk_generator.h"
#include "terrain.h"
#include "terrain_materials.h"
#include "multithreading/thread_memory_allocator.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_structs.h"
#include "util/rng.h"

#include <DirectXMath.h>

#include <numbers>
#include <vector>

using namespace glm;
using namespace DirectX;

#define DEFAULT_TEX_NUM_BLOCKS_X 32
#define DEFAULT_TEX_NUM_BLOCKS_Y 32

Chunk::Chunk(ivec2 chunkPos, Region* region)
    : chunkPos(chunkPos), region(region)
{}

void Chunk::setNeighbors(bool createNeighbors)
{
    const ivec2 thisRegionPosChunks = this->region->regionPosChunks;
    for (int dirIdx = 0; dirIdx < 4; ++dirIdx)
    {
        Chunk* neighborChunk = this->neighbors[dirIdx];
        if (neighborChunk != nullptr)
        {
            continue;
        }

        const NeighborDirection dir = static_cast<NeighborDirection>(dirIdx);
        const glm::ivec2 neighborChunkPos = this->chunkPos + neighborOffset(dir);

        Region* neighborRegion = this->region;
        const glm::ivec2 neighborChunkPos_region = neighborChunkPos - thisRegionPosChunks;
        if (glm::min(neighborChunkPos_region.x, neighborChunkPos_region.y) < 0 ||
            glm::max(neighborChunkPos_region.x, neighborChunkPos_region.y) >= regionSideLength)
        {
            neighborRegion = neighborRegion->getNeighbor(dir);
        }

        if (neighborRegion != nullptr)
        {
            neighborChunk = neighborRegion->getChunk(neighborChunkPos);

            bool needToSetNeighbor = true;
            if (createNeighbors && neighborChunk == nullptr)
            {
                neighborChunk = neighborRegion->createChunk(neighborChunkPos);
                neighborChunk->setNeighbors(false /*createNeighbors*/);
                needToSetNeighbor = false;
            }

            if (needToSetNeighbor && neighborChunk != nullptr)
            {
                this->setNeighbor(dir, neighborChunk); // also sets opposite direction

                // at this point, this chunk cannot have blocks, so we don't need to update
                // neighborChunk->numNeighborsWithBlocks
            }
        }
    }
}

void Chunk::setNeighbor(NeighborDirection dir, Chunk* neighborChunk)
{
    ASSERT(this->neighbors[static_cast<size_t>(dir)] == nullptr);

    this->neighbors[static_cast<size_t>(dir)] = neighborChunk;
    ++this->numNeighborsSet;
    neighborChunk->neighbors[static_cast<size_t>(oppositeNeighborDirection(dir))] = this;
    ++neighborChunk->numNeighborsSet;
}

void Chunk::generateTerrain(ThreadMemoryAllocator& threadMemoryAlloc)
{
    this->blocks.resize(numChunkBlocks);
    this->biomes.resize(chunkSizeXZSquare);

    const ivec2 chunkBlockPosXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);

    this->fillTerrainBlocksAndCreateStructures(threadMemoryAlloc);

    this->advanceState(ChunkState::HAS_TERRAIN);

    Terrain::setDirty();
}

void Chunk::checkStructureNeighbors()
{
    constexpr uint32_t sideLength = 2 * structureMaxChunkRadius + 1;
    constexpr uint32_t totalNumStructureNeighbors = sideLength * sideLength;
    this->structureNeighbors.reserve(totalNumStructureNeighbors);

    Chunk* corner = this;
    for (uint32_t i = 0; i < structureMaxChunkRadius; ++i)
    {
        corner = corner->neighbors[static_cast<size_t>(NeighborDirection::X_NEG)];
        ASSERT(corner != nullptr);
        corner = corner->neighbors[static_cast<size_t>(NeighborDirection::Z_NEG)];
        ASSERT(corner != nullptr);
    }

    bool setTerrainDirty = false;
    Chunk* rowStart = corner;
    for (uint32_t z = 0; z < sideLength; ++z)
    {
        Chunk* current = rowStart;
        for (uint32_t x = 0; x < sideLength; ++x)
        {
            this->structureNeighbors.push_back(current);

            const uint32_t neighborNumReady = current->numReadyStructureNeighbors.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (neighborNumReady == totalNumStructureNeighbors && current->getState() >= ChunkState::HAS_TERRAIN)
            {
                current->advanceState(ChunkState::NEEDS_FILL_STRUCTURES);
                setTerrainDirty = true;
            }

            if (x < sideLength - 1)
            {
                current = current->neighbors[static_cast<size_t>(NeighborDirection::X_POS)];
                ASSERT(current != nullptr);
            }
        }

        if (z < sideLength - 1)
        {
            rowStart = rowStart->neighbors[static_cast<size_t>(NeighborDirection::Z_POS)];
            ASSERT(rowStart != nullptr);
        }
    }

    if (setTerrainDirty)
    {
        Terrain::setDirty();
    }
}

void Chunk::fillStructuresAndDecorators()
{
    for (Chunk* structureNeighbor : this->structureNeighbors)
    {
        const std::vector<Structure>& neighborStructures = structureNeighbor->structures;
        this->fillStructureBlocks(neighborStructures.data(), neighborStructures.size());
    }

    RandomNumberGenerator decoratorRng = initRng(this->chunkPos.x, this->chunkPos.y /*z*/, 198594190);
    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const Biome biome = this->biomes[columnIdx];
            const Decorator& decorator = Biomes::getBiomeData(biome).decorator;

            if (decorator.isEmpty())
            {
                continue;
            }

            const uint baseBlockIdx = chunkSizeY * columnIdx;
            Block lastBlock = Block::BEDROCK;
            for (uint blockY = 0; blockY < chunkSizeY; ++blockY)
            {
                Block& thisBlock = this->blocks[baseBlockIdx + blockY];

                if (thisBlock == Block::AIR && lastBlock != Block::AIR)
                {
                    const Block decoratorBlock = decorator.getBlock(decoratorRng.nextFloat(), lastBlock);
                    if (decoratorBlock != Block::AIR)
                    {
                        thisBlock = decoratorBlock;
                    }
                }

                lastBlock = thisBlock;
            }
        }
    }

    this->advanceState(ChunkState::HAS_ALL_BLOCKS);

    bool setTerrainDirty = false;

    for (Chunk* neighborChunk : this->neighbors)
    {
        if (neighborChunk == nullptr)
        {
            continue;
        }

        const uint neighborNumNeighborsWithBlocks =
            neighborChunk->numNeighborsWithBlocks.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (neighborNumNeighborsWithBlocks == 4 && neighborChunk->getState() >= ChunkState::HAS_ALL_BLOCKS)
        {
            neighborChunk->advanceState(ChunkState::NEEDS_SEGMENTS);
            setTerrainDirty = true;
        }
    }

    if (this->numNeighborsWithBlocks.load(std::memory_order_acquire) == 4)
    {
        this->advanceState(ChunkState::NEEDS_SEGMENTS);
        setTerrainDirty = true;
    }

    if (setTerrainDirty)
    {
        Terrain::setDirty();
    }
}

bool Chunk::isRegionAllBlockType(const uvec3 startPos, const uvec3 endPos, BlockType blockType)
{
    for (uint blockZ = startPos.z; blockZ <= endPos.z; ++blockZ)
    {
        for (uint blockX = startPos.x; blockX <= endPos.x; ++blockX)
        {
            uint blockIdx = Chunk::blockPosXZToIdx(uvec2(blockX, blockZ));

            for (uint blockY = startPos.y; blockY <= endPos.y; ++blockY)
            {
                const Block block = this->blocks[blockIdx++];
                if (Blocks::getBlockData(block).type != blockType)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

bool Chunk::isSegmentSurroundedBySolid(const uvec3 startPos,
                                       const uvec3 endPos,
                                       const uvec3 chunkSegmentPos,
                                       const ChunkSegment* const prevSegments)
{
    // these two cases should be skipped by generateSegments()
    ASSERT(chunkSegmentPos.y != 0);
    ASSERT(chunkSegmentPos.y != numChunkSegmentsY - 1);

    const uint thisSegmentIdx = Chunk::segmentPosToIdx(chunkSegmentPos);

    // -x
    {
        Chunk* chunk;
        uint blockX;
        bool check = true;
        if (chunkSegmentPos.x == 0)
        {
            chunk = this->neighbors[static_cast<uint8_t>(NeighborDirection::X_NEG)];
            ASSERT(chunk != nullptr);
            blockX = chunkSizeXZ - 1;
        }
        else
        {
            chunk = this;
            blockX = startPos.x - 1;
            check = (prevSegments[thisSegmentIdx - numChunkSegmentsY] != ChunkSegment::SOLID_SURROUNDED);
        }

        if (check)
        {
            const bool isSolid = !chunk->isRegionAllBlockType(
                uvec3(blockX, startPos.y, startPos.z), uvec3(blockX, endPos.y, endPos.z), BlockType::SOLID);
            if (!isSolid)
            {
                return false;
            }
        }
    }

    // -y
    if (prevSegments[thisSegmentIdx - 1] != ChunkSegment::SOLID_SURROUNDED)
    {
        const uint blockY = startPos.y - 1;
        const bool isSolid = this->isRegionAllBlockType(
            uvec3(startPos.x, blockY, startPos.z), uvec3(endPos.x, blockY, endPos.z), BlockType::SOLID);
        if (!isSolid)
        {
            return false;
        }
    }

    // -z
    {
        Chunk* chunk;
        uint blockZ;
        bool check = true;
        if (chunkSegmentPos.z == 0)
        {
            chunk = this->neighbors[static_cast<uint8_t>(NeighborDirection::Z_NEG)];
            ASSERT(chunk != nullptr);
            blockZ = chunkSizeXZ - 1;
        }
        else
        {
            chunk = this;
            blockZ = startPos.z - 1;
            check = (prevSegments[thisSegmentIdx - (numChunkSegmentsXZ * numChunkSegmentsY)] != ChunkSegment::SOLID_SURROUNDED);
        }

        if (check)
        {
            const bool isSolid = chunk->isRegionAllBlockType(
                uvec3(startPos.x, startPos.y, blockZ), uvec3(endPos.x, endPos.y, blockZ), BlockType::SOLID);
            if (!isSolid)
            {
                return false;
            }
        }
    }

    { // +x
        Chunk* chunk;
        uint blockX;
        if (chunkSegmentPos.x == numChunkSegmentsXZ - 1)
        {
            chunk = this->neighbors[static_cast<uint8_t>(NeighborDirection::X_POS)];
            ASSERT(chunk != nullptr);
            blockX = 0;
        }
        else
        {
            chunk = this;
            blockX = endPos.x;
        }

        const bool isSolid = chunk->isRegionAllBlockType(
            uvec3(blockX, startPos.y, startPos.z), uvec3(blockX, endPos.y, endPos.z), BlockType::SOLID);
        if (!isSolid)
        {
            return false;
        }
    }

    // +y
    {
        const uint blockY = endPos.y + 1;
        const bool isSolid = this->isRegionAllBlockType(
            uvec3(startPos.x, blockY, startPos.z), uvec3(endPos.x, blockY, endPos.z), BlockType::SOLID);
        if (!isSolid)
        {
            return false;
        }
    }

    { // +z
        Chunk* chunk;
        uint blockZ;
        if (chunkSegmentPos.z == numChunkSegmentsXZ - 1)
        {
            chunk = this->neighbors[static_cast<uint8_t>(NeighborDirection::Z_POS)];
            ASSERT(chunk != nullptr);
            blockZ = 0;
        }
        else
        {
            chunk = this;
            blockZ = endPos.z;
        }

        const bool isSolid = chunk->isRegionAllBlockType(
            uvec3(startPos.x, startPos.y, blockZ), uvec3(endPos.x, endPos.y, blockZ), BlockType::SOLID);
        if (!isSolid)
        {
            return false;
        }
    }

    return true;
}

void Chunk::generateSegments(ThreadMemoryAllocator& threadMemoryAlloc)
{
    ChunkSegment* prevSegments = threadMemoryAlloc.request<ChunkSegment>(numChunkSegments);
    uint32_t segmentIdx = 0;

    // reserve space for at least bottom layer and top surface layer
    this->segmentsToGenerate.reserve(numChunkSegmentsXZ * numChunkSegmentsXZ * 2);

    for (uint segmentZ = 0; segmentZ < numChunkSegmentsXZ; ++segmentZ)
    {
        for (uint segmentX = 0; segmentX < numChunkSegmentsXZ; ++segmentX)
        {
            for (uint segmentY = 0; segmentY < numChunkSegmentsY; ++segmentY)
            {
                const uvec3 chunkSegmentPos(segmentX, segmentY, segmentZ);
                uvec3 segmentStartPos, segmentEndPos;
                Chunk::segmentPosToBounds(chunkSegmentPos, segmentStartPos, segmentEndPos);

                ChunkSegment segment = ChunkSegment::MIXED;
                const Block blockAtBasePos = this->blocks[Chunk::blockPosToIdx(segmentStartPos)];
                switch (Blocks::getBlockData(blockAtBasePos).type)
                {
                    case BlockType::AIR:
                    {
                        if (this->isRegionAllBlockType(segmentStartPos, segmentEndPos, BlockType::AIR))
                        {
                            segment = ChunkSegment::AIR;
                        }
                        break;
                    }
                    case BlockType::SOLID:
                    {
                        // top and bottom chunks cannot be surrounded
                        const bool isTopOrBottom = segmentY == 0 || segmentY == numChunkSegmentsY - 1;
                        if (!isTopOrBottom &&
                            this->isRegionAllBlockType(segmentStartPos, segmentEndPos, BlockType::SOLID) &&
                            isSegmentSurroundedBySolid(segmentStartPos, segmentEndPos, chunkSegmentPos, prevSegments))
                        {
                            segment = ChunkSegment::SOLID_SURROUNDED;
                        }
                        break;
                    }
                }

                prevSegments[segmentIdx++] = segment; // used for easier condition checking for future segments in this function
                this->segmentsToGenerate.push_back(chunkSegmentPos);
            }
        }
    }

    this->advanceState(ChunkState::NEEDS_GEOMETRY);
    Terrain::setDirty();
}

static inline DirectX::XMFLOAT2 vec2ToDirectX(const glm::vec2& v)
{
    return { v.x, v.y };
}

static inline DirectX::XMFLOAT3 vec3ToDirectX(const glm::vec3& v)
{
    return { v.x, v.y, v.z };
}

bool Chunk::shouldGenerateFace(ivec3 thisPos_CS, BlockType thisBlockType, ivec3 neighborPos_CS, int faceIdx)
{
    ASSERT(thisBlockType != BlockType::AIR); // AIR should be skipped before this function is even called

    if (neighborPos_CS.y < 0 || neighborPos_CS.y >= chunkSizeY)
    {
        return true;
    }

    Block neighborBlock;

    if (!Chunk::isPosInBounds(ivec2(neighborPos_CS.x, neighborPos_CS.z)))
    {
        const Chunk* neighborChunk = this->neighbors[faceIdx]; // faceIdx 0-3 corresponds to NeighborDirection
        ASSERT(neighborChunk != nullptr); // neighborChunk should exist because this function is not called until all neighbors have blocks
        const ivec3 pos_neighborCS = {
            (neighborPos_CS.x + chunkSizeXZ) & (chunkSizeXZ - 1),
            neighborPos_CS.y,
            (neighborPos_CS.z + chunkSizeXZ) & (chunkSizeXZ - 1),
        };
        neighborBlock = neighborChunk->blocks[Chunk::blockPosToIdx(uvec3(pos_neighborCS))];
    }
    else
    {
        neighborBlock = blocks[Chunk::blockPosToIdx(uvec3(neighborPos_CS))];
    }

    const BlockType neighborBlockType = Blocks::getBlockData(neighborBlock).type;
    if (neighborBlockType == BlockType::AIR)
    {
        return true;
    }

    switch (thisBlockType)
    {
        case BlockType::SOLID:
            return neighborBlockType != BlockType::SOLID;
        case BlockType::TRANSPARENT_CUTOUT:
        {
            if (neighborBlockType == BlockType::SOLID)
            {
                return false;
            }

            ASSERT(neighborBlockType == BlockType::TRANSPARENT_CUTOUT);
            return all(lessThanEqual(thisPos_CS, neighborPos_CS)); // prevents overlapping faces
        }
    }

    ASSERT(false); // this should not be reachable
    return false;
}

// first four match NeighborDirection enum
inline constexpr ivec3 faceOffsets[6] = {
    ivec3(1, 0, 0),  // +x
    ivec3(0, 0, 1),  // +z
    ivec3(-1, 0, 0), // -x
    ivec3(0, 0, -1), // -z
    ivec3(0, 1, 0),  // +y
    ivec3(0, -1, 0), // -y
};

inline constexpr ivec3 cubeFaceVertPositions[24] = {
    ivec3(1, 1, 0), ivec3(1, 1, 1), ivec3(1, 0, 1), ivec3(1, 0, 0), // +x
    ivec3(1, 1, 1), ivec3(0, 1, 1), ivec3(0, 0, 1), ivec3(1, 0, 1), // +z
    ivec3(0, 1, 1), ivec3(0, 1, 0), ivec3(0, 0, 0), ivec3(0, 0, 1), // -x
    ivec3(0, 1, 0), ivec3(1, 1, 0), ivec3(1, 0, 0), ivec3(0, 0, 0), // -z
    ivec3(1, 1, 1), ivec3(1, 1, 0), ivec3(0, 1, 0), ivec3(0, 1, 1), // +y
    ivec3(0, 0, 1), ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(1, 0, 1), // -y
};

inline constexpr float halfInvSqrt2 = 0.5f / std::numbers::sqrt2_v<float>;
inline constexpr float xShapeMin = 0.5f - halfInvSqrt2;
inline constexpr float xShapeMax = 0.5f + halfInvSqrt2;
inline constexpr vec3 xShapedFaceVertPositions[8] = {
    vec3(xShapeMax, 1.f, xShapeMax), vec3(xShapeMin, 1.f, xShapeMin), vec3(xShapeMin, 0.f, xShapeMin), vec3(xShapeMax, 0.f, xShapeMax),
    vec3(xShapeMin, 1.f, xShapeMax), vec3(xShapeMax, 1.f, xShapeMin), vec3(xShapeMax, 0.f, xShapeMin), vec3(xShapeMin, 0.f, xShapeMax),
};
inline constexpr vec3 xShapedFaceNormals[2] = {
    vec3(-halfInvSqrt2, 0.f, halfInvSqrt2),
    vec3(halfInvSqrt2, 0.f, halfInvSqrt2),
};

inline constexpr uvec2 uvOffsets[4] = {
    uvec2(1, 0),
    uvec2(0, 0),
    uvec2(0, 1),
    uvec2(1, 1),
};
inline constexpr vec2 uvMultiplier = 1.f / vec2(DEFAULT_TEX_NUM_BLOCKS_X, DEFAULT_TEX_NUM_BLOCKS_Y);

void Chunk::setInstance(Instance* instance)
{
    this->instance = instance;
    this->instance->setVisible(this->getIsInstanceVisible());
}

void Chunk::createInstance()
{
    std::vector<Vertex>& verts = this->instance->host_verts;
    std::vector<uint32_t>& idxs = this->instance->host_idxs;
    std::vector<uint32_t> emissiveTriangleIdxs;

    constexpr size_t numVertsToReserve = 1 << 15;
    verts.reserve(numVertsToReserve);
    idxs.reserve(numVertsToReserve * 6 / 4);
    emissiveTriangleIdxs.reserve(512);

    for (const uvec3& segmentPos : this->segmentsToGenerate)
    {
        uvec3 segmentStartPos, segmentEndPos;
        Chunk::segmentPosToBounds(segmentPos, segmentStartPos, segmentEndPos);

        for (uint blockZ = segmentStartPos.z; blockZ <= segmentEndPos.z; ++blockZ)
        {
            for (uint blockX = segmentStartPos.x; blockX <= segmentEndPos.x; ++blockX)
            {
                const uint baseBlockIdx = Chunk::blockPosXZToIdx(uvec2(blockX, blockZ));

                for (uint blockY = segmentStartPos.y; blockY <= segmentEndPos.y; ++blockY)
                {
                    const uvec3 blockPos_CS(blockX, blockY, blockZ);
                    const uint blockIdx = baseBlockIdx + blockY;
                    const Block block = blocks[blockIdx];
                    if (block == Block::AIR)
                    {
                        continue;
                    }

                    const BlockData& blockData = Blocks::getBlockData(block);

                    if (blockData.shape == BlockShape::X_SHAPED)
                    {
                        const uint baseVertIdx = static_cast<uint>(verts.size());

                        const uvec2 baseTexCoords = blockData.uvs[1]; // side
                        for (uint i = 0; i < 8; ++i)
                        {
                            // TODO: random jitter offset
                            const vec3 vertPos_CS = vec3(blockPos_CS) + xShapedFaceVertPositions[i];
                            const vec3 normal = xShapedFaceNormals[i / 4];
                            const vec2 uv = vec2(baseTexCoords + uvOffsets[i % 4]) * uvMultiplier;
                            verts.emplace_back(
                                vec3ToDirectX(vertPos_CS),
                                vec3ToDirectX(normal),
                                vec2ToDirectX(uv)
                            );
                        }

                        for (uint j = 0; j < 2; ++j)
                        {
                            const uint offset = j * 4;
                            idxs.emplace_back(baseVertIdx + offset + 0u);
                            idxs.emplace_back(baseVertIdx + offset + 1u);
                            idxs.emplace_back(baseVertIdx + offset + 2u);
                            idxs.emplace_back(baseVertIdx + offset + 0u);
                            idxs.emplace_back(baseVertIdx + offset + 2u);
                            idxs.emplace_back(baseVertIdx + offset + 3u);
                        }
                        continue;
                    }
                    else // if (blockData.shape == BlockShape::CUBE)
                    {
                        for (uint faceIdx = 0; faceIdx < 6; ++faceIdx)
                        {
                            const ivec3 neighborOffset = faceOffsets[faceIdx];
                            const ivec3 neighborPos_CS = ivec3(blockPos_CS) + neighborOffset;

                            if (!shouldGenerateFace(blockPos_CS, blockData.type, neighborPos_CS, faceIdx))
                            {
                                continue;
                            }

                            const uint baseVertIdx = static_cast<uint>(verts.size());

                            const DirectX::XMFLOAT3 normal = vec3ToDirectX(vec3(neighborOffset));
                            const ivec3* thisFaceVertPositions = cubeFaceVertPositions + (faceIdx * 4);
                            for (uint i = 0; i < 4; ++i)
                            {
                                const vec3 vertPos_CS = vec3(ivec3(blockPos_CS) + thisFaceVertPositions[i]);

                                const uvec2 baseTexCoords = blockData.uvs[glm::max(static_cast<int>(faceIdx) - 3, 0)];
                                const vec2 uv = (vec2(baseTexCoords + uvOffsets[i])) * uvMultiplier;

                                verts.emplace_back(vec3ToDirectX(vertPos_CS), normal, vec2ToDirectX(uv));
                            }

                            const uint32_t triangleIdx = static_cast<uint32_t>(idxs.size() / 3u);

                            idxs.emplace_back(baseVertIdx + 0u);
                            idxs.emplace_back(baseVertIdx + 1u);
                            idxs.emplace_back(baseVertIdx + 2u);
                            idxs.emplace_back(baseVertIdx + 0u);
                            idxs.emplace_back(baseVertIdx + 2u);
                            idxs.emplace_back(baseVertIdx + 3u);

                            if (blockData.emitsLight)
                            {
                                emissiveTriangleIdxs.emplace_back(triangleIdx);
                                emissiveTriangleIdxs.emplace_back(triangleIdx + 1u);
                            }
                        }
                    }
                }
            }
        }
    }

    ASSERT(verts.size() > 0);
    ASSERT(idxs.size() > 0);

    const ivec2 chunkBlockPos_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);
    instance->setTransformOffset(ivec3(chunkBlockPos_WS.x, 0, chunkBlockPos_WS.y /*z*/));

    instance->finalizeGeometry();

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
    this->setState(ChunkState::NEEDS_GEOMETRY);
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

bool Chunk::advanceState(ChunkState newState)
{
    ChunkState expected = this->state.load(std::memory_order_acquire);

    while (expected < newState)
    {
        if (this->state.compare_exchange_weak(expected, newState, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true; // this thread advanced the state
        }
        // on failure, expected is updated; loop continues if still < newState
    }

    return false; // already >= newState, or another thread advanced it
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

uint32_t Chunk::getNumNeighborsSet() const
{
    return this->numNeighborsSet;
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
uint32_t Chunk::blockPosToIdx(uvec3 chunkBlockPos)
{
    return chunkBlockPos.y + chunkSizeY * (chunkBlockPos.x + chunkSizeXZ * (chunkBlockPos.z));
}

uint32_t Chunk::blockPosXZToIdx(uvec2 chunkBlockPos)
{
    return chunkSizeY * (chunkBlockPos.x + chunkSizeXZ * (chunkBlockPos.y /*z*/));
}

uint32_t Chunk::segmentPosToIdx(uvec3 chunkSegmentPos)
{
    return chunkSegmentPos.y + numChunkSegmentsY * (chunkSegmentPos.x + numChunkSegmentsXZ * (chunkSegmentPos.z));
}

void Chunk::segmentPosToBounds(uvec3 chunkSegmentPos, uvec3& outSegmentStartPos, uvec3& outSegmentEndPos)
{
    outSegmentStartPos = chunkSegmentPos * uvec3(chunkSegmentSizeXZ, chunkSegmentSizeY, chunkSegmentSizeXZ);
    outSegmentEndPos = outSegmentStartPos + uvec3(chunkSegmentSizeXZ - 1, chunkSegmentSizeY - 1, chunkSegmentSizeXZ - 1);
}

bool Chunk::isPosInBounds(glm::ivec2 posXZ_CS)
{
    return std::min(posXZ_CS.x, posXZ_CS.y /*z*/) >= 0 && std::max(posXZ_CS.x, posXZ_CS.y /*z*/) < chunkSizeXZ;
}

Region::Region(glm::ivec2 regionPos)
    : regionPos(regionPos), regionPosChunks(regionPos * static_cast<int>(regionSideLength))
{}

Chunk* Region::getChunk(ivec2 chunkPos)
{
    return this->chunks[chunkPosToIdx(chunkPos - this->regionPosChunks)].get();
}

Chunk* Region::createChunk(ivec2 chunkPos)
{
    const uint chunkIdx = chunkPosToIdx(chunkPos - this->regionPosChunks);
    this->chunks[chunkIdx] = std::make_unique<Chunk>(chunkPos, this);
    return this->chunks[chunkIdx].get();
}

Chunk* Region::getOrCreateChunk(ivec2 chunkPos)
{
    const uint chunkIdx = chunkPosToIdx(chunkPos - this->regionPosChunks);
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
    ++this->numNeighborsSet;
    neighborRegion->neighbors[static_cast<size_t>(oppositeNeighborDirection(dir))] = this;
    ++neighborRegion->numNeighborsSet;
}

uint32_t Region::getNumNeighborsSet() const
{
    return this->numNeighborsSet;
}

// x changes fastest, then z
//
// for loops should be written like this:
// for (uint z = 0; z < REGION_SIDE_LENGTH; ++z)
// {
//     for (uint x = 0; x < REGION_SIDE_LENGTH; ++x)
//     {
//         // do stuff here
uint32_t Region::chunkPosToIdx(ivec2 regionChunkPos)
{
    return regionChunkPos.x + regionSideLength * (regionChunkPos.y /*z*/);
}
