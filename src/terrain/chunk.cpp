// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "chunk.h"

#include "block.h"
#include "block_model.h"
#include "cave_biome.h"
#include "terrain.h"
#include "terrain_materials.h"
#include "terrain_omm.h"
#include "multithreading/thread_memory_allocator.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_structs.h"
#include "settings_manager.h"
#include "util/packing.h"
#include "util/rng.h"

#include <DirectXMath.h>

#include <numbers>
#include <vector>

using namespace glm;
using namespace DirectX;

namespace
{
inline constexpr uint8_t NO_CAVE_BIOME = 0xff;

// First four match NeighborDirection. This is also the three-bit SURFACE_MOUNT state encoding.
inline constexpr ivec3 faceOffsets[6] = {
    ivec3(1, 0, 0),  // +x
    ivec3(0, 0, 1),  // +z
    ivec3(-1, 0, 0), // -x
    ivec3(0, 0, -1), // -z
    ivec3(0, 1, 0),  // +y
    ivec3(0, -1, 0), // -y
};

inline constexpr vec3 faceTangentX[6] = {
    vec3(0, -1, 0), vec3(1, 0, 0), vec3(0, 1, 0),
    vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0),
};
inline constexpr vec3 faceTangentZ[6] = {
    vec3(0, 0, 1), vec3(0, -1, 0), vec3(0, 0, 1),
    vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, -1),
};

constexpr uint8_t surfaceForFace(uint8_t face)
{
    return face < 4 ? DECORATOR_SURFACE_WALL
                    : (face == 4 ? DECORATOR_SURFACE_FLOOR : DECORATOR_SURFACE_CEILING);
}
} // namespace

Chunk::Chunk(ivec2 chunkPos, Region* region)
    : chunkPos(chunkPos), region(region)
{}

// Main thread only: this can call Region::createChunk, which mutates Region::chunks
// without locking. Other code (e.g. Terrain::exportWorld) iterates Region::chunks
// concurrently with worker tasks and relies on the array not being mutated under
// it. If this assumption is ever broken, the iteration sites need locking.
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
    if (!this->wasImported)
    {
        this->blocks.resize(numChunkBlocks);
        this->biomes.resize(chunkSizeXZSquare);
        this->terrainTopY.resize(chunkSizeXZSquare);
        this->caveBiomes.assign(numChunkBlocks, NO_CAVE_BIOME);

        this->fillTerrainBlocksAndCreateStructures(threadMemoryAlloc);
    }
    this->buildTerrainAirMask();

    this->advanceState(ChunkState::HAS_TERRAIN);

    Terrain::setDirty();
}

void Chunk::buildTerrainAirMask()
{
    constexpr uint32_t wordsPerColumn = chunkSizeY / 64;
    this->terrainAirMask.assign(chunkSizeXZSquare * wordsPerColumn, 0);
    for (uint32_t blockIdx = 0; blockIdx < numChunkBlocks; ++blockIdx)
    {
        if (this->blocks[blockIdx] == Block::AIR)
        {
            this->terrainAirMask[blockIdx / 64] |= uint64_t(1) << (blockIdx % 64);
        }
    }
}

bool Chunk::isTerrainAir_WS(glm::ivec3 pos_WS) const
{
    if (pos_WS.y < 0 || pos_WS.y >= static_cast<int>(chunkSizeY))
    {
        return false;
    }

    const glm::ivec2 posChunk(MathUtil::floorDiv(pos_WS.x, chunkSizeXZ), MathUtil::floorDiv(pos_WS.z, chunkSizeXZ));
    const glm::ivec2 chunkOffset = posChunk - this->chunkPos;
    constexpr int radius = static_cast<int>(structureMaxChunkRadius);
    ASSERT(glm::abs(chunkOffset.x) <= radius && glm::abs(chunkOffset.y) <= radius, "position outside structure neighborhood");
    constexpr int sideLength = 2 * radius + 1;
    const Chunk* chunk = this->structureNeighbors[(chunkOffset.y + radius) * sideLength + (chunkOffset.x + radius)];

    const glm::ivec2 chunkOriginXZ_WS = posChunk * static_cast<int>(chunkSizeXZ);
    const uint32_t blockIdx =
        blockPosToIdx(glm::uvec3(pos_WS.x - chunkOriginXZ_WS.x, pos_WS.y, pos_WS.z - chunkOriginXZ_WS.y /*z*/));
    return (chunk->terrainAirMask[blockIdx / 64] >> (blockIdx % 64)) & 1;
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

void Chunk::runStructuresAndDecoratorPass()
{
    for (const Chunk* structureNeighbor : this->structureNeighbors)
    {
        const std::vector<Structure>& neighborStructures = structureNeighbor->structures;
        this->fillStructureBlocks(neighborStructures.data(), neighborStructures.size());
    }

    // Cave structures fill one type at a time in enum order so a type's blocks are all in place
    // before a lower-priority type (e.g. vines) reads the world around it
    for (uint32_t typeIdx = 0; typeIdx < static_cast<uint32_t>(CaveStructureType::COUNT); ++typeIdx)
    {
        for (const Chunk* structureNeighbor : this->structureNeighbors)
        {
            const std::vector<CaveStructure>& neighborCaveStructures = structureNeighbor->caveStructures;
            this->fillCaveStructureBlocks(
                neighborCaveStructures.data(), neighborCaveStructures.size(), static_cast<CaveStructureType>(typeIdx));
        }
    }

    const uint worldSeed = SettingsManager::getWorldSeed();
    RandomNumberGenerator decoratorRng = initRng(worldSeed ^ hash(198594190), this->chunkPos.x, this->chunkPos.y /*z*/);
    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const uint columnIdx = blockX + chunkSizeXZ * blockZ;

            const Biome biome = this->biomes[columnIdx];
            const Decorator& decorator = Biomes::getBiomeData(biome).decorator;

            const uint baseBlockIdx = chunkSizeY * columnIdx;
            const uint terrainTopY = this->terrainTopY[columnIdx];
            Block bottomBlock = Block::BEDROCK;
            for (uint blockY = 0; blockY < chunkSizeY; ++blockY)
            {
                Block& thisBlock = this->blocks[baseBlockIdx + blockY];

                // Decorators only stand on full cubes, never on other decorators or structure flora
                if (thisBlock == Block::AIR && bottomBlock != Block::AIR &&
                    Blocks::getBlockData(bottomBlock).shape == BlockShape::CUBE)
                {
                    const uint groundY = blockY - 1;
                    Block decoratorBlock = Block::AIR;
                    // Cave-air cells are handled by the all-face pass below. Everything else at or
                    // above terrain top is the ordinary surface-biome floor pass.
                    if (this->caveBiomes[baseBlockIdx + blockY] == NO_CAVE_BIOME &&
                        groundY >= terrainTopY && !decorator.isEmpty())
                    {
                        decoratorBlock = decorator.getBlock(
                            decoratorRng.nextFloat(), bottomBlock, DECORATOR_SURFACE_FLOOR);
                    }
                    if (decoratorBlock != Block::AIR)
                    {
                        thisBlock = decoratorBlock;
                    }
                }

                bottomBlock = thisBlock;
            }
        }
    }

    const ivec2 chunkOriginXZ_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);
    const auto getBlock = [&](ivec3 pos_CS) -> Block
    {
        if (pos_CS.y < 0 || pos_CS.y >= static_cast<int>(chunkSizeY)) return Block::AIR;
        if (Chunk::isInChunkXZ(pos_CS)) return this->blocks[Chunk::blockPosToIdx(uvec3(pos_CS))];

        int face = pos_CS.x < 0 ? 2 : (pos_CS.x >= static_cast<int>(chunkSizeXZ) ? 0
                                  : (pos_CS.z < 0 ? 3 : 1));
        const Chunk* neighbor = this->neighbors[face];
        ASSERT(neighbor != nullptr);
        const ivec3 neighborPos_CS = {
            (pos_CS.x + chunkSizeXZ) & (chunkSizeXZ - 1),
            pos_CS.y,
            (pos_CS.z + chunkSizeXZ) & (chunkSizeXZ - 1),
        };
        return neighbor->blocks[Chunk::blockPosToIdx(uvec3(neighborPos_CS))];
    };

    for (uint blockZ = 0; blockZ < chunkSizeXZ; ++blockZ)
    {
        for (uint blockX = 0; blockX < chunkSizeXZ; ++blockX)
        {
            const uint baseBlockIdx = Chunk::blockPosXZToIdx(uvec2(blockX, blockZ));
            for (uint blockY = 1; blockY + 1 < chunkSizeY; ++blockY)
            {
                const uint blockIdx = baseBlockIdx + blockY;
                const uint8_t caveBiomeValue = this->caveBiomes[blockIdx];
                if (caveBiomeValue == NO_CAVE_BIOME || this->blocks[blockIdx] != Block::AIR) continue;

                const CaveBiome caveBiome = static_cast<CaveBiome>(caveBiomeValue);
                ASSERT(caveBiome < CaveBiome::COUNT);
                const Decorator& caveDecorator = CaveBiomes::getCaveBiomeData(caveBiome).decorator;
                if (caveDecorator.isEmpty()) continue;

                const ivec3 blockPos_CS(blockX, blockY, blockZ);
                const ivec3 blockPos_WS(chunkOriginXZ_WS.x + blockX, blockY, chunkOriginXZ_WS.y + blockZ);
                uint8_t candidateFaces[6];
                uint8_t numCandidateFaces = 0;
                for (uint8_t face = 0; face < 6; ++face)
                {
                    const uint8_t surface = surfaceForFace(face);
                    const ivec3 supportPos_CS = blockPos_CS - faceOffsets[face];
                    const ivec3 supportPos_WS = blockPos_WS - faceOffsets[face];
                    // Structure workers only write terrain-air cells. Reject those through the
                    // immutable mask before reading a neighbor block that may be filling concurrently.
                    if (this->isTerrainAir_WS(supportPos_WS)) continue;
                    const Block supportBlock = getBlock(supportPos_CS);
                    const BlockData& supportData = Blocks::getBlockData(supportBlock);
                    if (supportData.type == BlockType::SOLID && supportData.shape == BlockShape::CUBE &&
                        caveDecorator.supportsSurface(surface, supportBlock))
                    {
                        candidateFaces[numCandidateFaces++] = face;
                    }
                }
                if (numCandidateFaces == 0) continue;

                auto faceRng = initRng(worldSeed ^ hash(0x7A11FACEu), blockPos_WS.x, blockPos_WS.y, blockPos_WS.z);
                const uint8_t face = candidateFaces[faceRng.nextUint() % numCandidateFaces];
                const Block supportBlock = getBlock(blockPos_CS - faceOffsets[face]);
                auto blockRng = initRng(worldSeed ^ hash(771093284), blockPos_WS.x, blockPos_WS.y, blockPos_WS.z);
                const Block decoratorBlock = caveDecorator.getBlock(
                    blockRng.nextFloat(), supportBlock, surfaceForFace(face));
                if (decoratorBlock == Block::AIR) continue;

                this->blocks[blockIdx] = decoratorBlock;
                if (Blocks::getBlockData(decoratorBlock).stateKind == BlockStateKind::SURFACE_MOUNT)
                {
                    this->blockStates.insert_or_assign(blockIdx, face);
                }
            }
        }
    }
}

void Chunk::fillStructuresAndDecorators()
{
    if (!this->wasImported)
    {
        this->runStructuresAndDecoratorPass();
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

bool Chunk::isRegionAllBlockType(const uvec3 startPos, const uvec3 endPos, BlockType blockType, BlockShape blockShape)
{
    for (uint blockZ = startPos.z; blockZ <= endPos.z; ++blockZ)
    {
        for (uint blockX = startPos.x; blockX <= endPos.x; ++blockX)
        {
            uint blockIdx = Chunk::blockPosToIdx(uvec3(blockX, startPos.y, blockZ));

            for (uint blockY = startPos.y; blockY <= endPos.y; ++blockY)
            {
                const BlockData& blockData = Blocks::getBlockData(this->blocks[blockIdx++]);
                if ((blockData.type != blockType) || (blockShape != BlockShape::COUNT && blockData.shape != blockShape))
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
            const bool isSolid = chunk->isRegionAllBlockType(
                uvec3(blockX, startPos.y, startPos.z), uvec3(blockX, endPos.y, endPos.z), BlockType::SOLID, BlockShape::CUBE);
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
            uvec3(startPos.x, blockY, startPos.z), uvec3(endPos.x, blockY, endPos.z), BlockType::SOLID, BlockShape::CUBE);
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
                uvec3(startPos.x, startPos.y, blockZ), uvec3(endPos.x, endPos.y, blockZ), BlockType::SOLID, BlockShape::CUBE);
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
            blockX = endPos.x + 1;
        }

        const bool isSolid = chunk->isRegionAllBlockType(
            uvec3(blockX, startPos.y, startPos.z), uvec3(blockX, endPos.y, endPos.z), BlockType::SOLID, BlockShape::CUBE);
        if (!isSolid)
        {
            return false;
        }
    }

    // +y
    {
        const uint blockY = endPos.y + 1;
        const bool isSolid = this->isRegionAllBlockType(
            uvec3(startPos.x, blockY, startPos.z), uvec3(endPos.x, blockY, endPos.z), BlockType::SOLID, BlockShape::CUBE);
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
            blockZ = endPos.z + 1;
        }

        const bool isSolid = chunk->isRegionAllBlockType(
            uvec3(startPos.x, startPos.y, blockZ), uvec3(endPos.x, endPos.y, blockZ), BlockType::SOLID, BlockShape::CUBE);
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
                            this->isRegionAllBlockType(segmentStartPos, segmentEndPos, BlockType::SOLID, BlockShape::CUBE) &&
                            isSegmentSurroundedBySolid(segmentStartPos, segmentEndPos, chunkSegmentPos, prevSegments))
                        {
                            segment = ChunkSegment::SOLID_SURROUNDED;
                        }
                        break;
                    }
                }

                prevSegments[segmentIdx++] = segment; // used for easier condition checking for future segments in this function
                if (segment != ChunkSegment::AIR && segment != ChunkSegment::SOLID_SURROUNDED)
                {
                    this->segmentsToGenerate.push_back(chunkSegmentPos);
                }
            }
        }
    }

    this->advanceState(ChunkState::NEEDS_GEOMETRY);
    Terrain::setDirty();
}

static inline DirectX::XMFLOAT3 vec3ToDirectX(const glm::vec3& v)
{
    return { v.x, v.y, v.z };
}

static inline Vertex makeVertex(const glm::vec3& pos, const glm::vec3& nor, const glm::vec2& uv)
{
    return { vec3ToDirectX(pos), Util::octEncode(vec3ToDirectX(nor)), Util::packFloat2ToUint(uv.x, uv.y) };
}

bool Chunk::shouldGenerateFace(ivec3 thisPos_CS, BlockType thisBlockType, BlockShape thisBlockShape, ivec3 neighborPos_CS, int faceIdx)
{
    ASSERT(thisBlockType != BlockType::AIR); // AIR should be skipped before this function is even called

    if (neighborPos_CS.y < 0 || neighborPos_CS.y >= chunkSizeY)
    {
        return true;
    }

    Block neighborBlock;

    if (!Chunk::isInChunkXZ(neighborPos_CS))
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

    const BlockData& neighborBlockData = Blocks::getBlockData(neighborBlock);
    return blockFaceVisible(thisBlockType, thisBlockShape, neighborBlockData.type,
                            neighborBlockData.shape, faceIdx);
}

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

void Chunk::setInstances(Instance* terrainInstance, Instance* waterInstance)
{
    this->terrainInstance = terrainInstance;
    this->waterInstance = waterInstance;
    this->setInstancesVisible(this->areInstancesVisible);
}

static PerTriangleData makeBlockTriangleData(const BlockData& block, uint32_t slice)
{
    PerTriangleData data{};
    data.texArraySliceIdx = slice;
    if (TerrainMaterials::sliceHasBiomeTint(slice)) data.flags |= TRIANGLE_FLAG_BIOME_TINT;
    if (block.translucent) data.flags |= TRIANGLE_FLAG_DIFFUSE_TRANSMISSION;
    if (block.proceduralColor) data.flags |= TRIANGLE_FLAG_PROCEDURAL_COLOR;
    if (block.type == BlockType::GLASS) data.flags |= TRIANGLE_FLAG_IS_GLASS;
    return data;
}

void Chunk::createInstances()
{
    std::vector<Vertex>& terrainVerts = this->terrainInstance->host_verts;
    std::vector<uint32_t>& terrainIdxs = this->terrainInstance->host_idxs;
    std::vector<PerTriangleData>& terrainPerTriDatas = this->terrainInstance->host_perTriDatas;
    std::vector<uint16_t>& terrainOmmIdxs = this->terrainInstance->host_ommIdxs;
    std::vector<uint32_t> terrainEmissiveTriangleIdxs;
    std::vector<Vertex>& waterVerts = this->waterInstance->host_verts;
    std::vector<uint32_t>& waterIdxs = this->waterInstance->host_idxs;
    std::vector<PerTriangleData>& waterPerTriDatas = this->waterInstance->host_perTriDatas;

    constexpr size_t numTerrainVertsToReserve = 1 << 14; // approximate size
    terrainVerts.reserve(numTerrainVertsToReserve);
    terrainIdxs.reserve(numTerrainVertsToReserve * 6 / 4);
    terrainPerTriDatas.reserve(numTerrainVertsToReserve / 2);

    constexpr size_t numWaterVertsToReserve = 1 << 8;
    waterVerts.reserve(numWaterVertsToReserve);
    waterIdxs.reserve(numWaterVertsToReserve * 6 / 4);
    waterPerTriDatas.reserve(numWaterVertsToReserve / 2);

    terrainEmissiveTriangleIdxs.reserve(512);

    const uint worldSeed = SettingsManager::getWorldSeed();

    const bool useOmms = TerrainOmm::isBaked();
    bool hasCutoutFaces = false;
    const auto appendOmmIdxs = [&](const uint32_t texArraySliceIdx, const uint numTris)
    {
        const bool isCutout = TerrainOmm::texArraySliceHasCutout(texArraySliceIdx);
        hasCutoutFaces |= isCutout;
        for (uint t = 0; t < numTris; ++t)
        {
            terrainOmmIdxs.emplace_back(isCutout ? TerrainOmm::getOmmIdx(texArraySliceIdx, t % 2)
                                                 : TerrainOmm::OMM_IDX_FULLY_OPAQUE);
        }
    };

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

                    if (blockData.shape == BlockShape::DECORATOR_CUSTOM)
                    {
                        const auto& model = BlockModels::get(blockData.modelIdx);
                        const ivec2 columnPos_WS = this->chunkPos * static_cast<int>(chunkSizeXZ) + ivec2(blockX, blockZ);
                        // Independent of jitter and traversal order, and includes Y for cave layers.
                        auto rng = initRng(worldSeed ^ 0xB16B00B5u, static_cast<uint>(columnPos_WS.x),
                                           blockY, static_cast<uint>(columnPos_WS.y));
                        const uint turn = blockData.rotationY[rng.nextUint() % blockData.numRotationsY];
                        uint8_t mountFace = 4;
                        if (blockData.stateKind == BlockStateKind::SURFACE_MOUNT)
                        {
                            const auto state = this->blockStates.find(blockIdx);
                            if (state != this->blockStates.end()) mountFace = state->second & 0x7u;
                            ASSERT(mountFace < 6);
                        }
                        const vec3 mountNormal(faceOffsets[mountFace]);
                        vec3 jitter(0.f);
                        if (blockData.randomJitter)
                        {
                            auto jitterRng = initRng(worldSeed ^ hash(392421012),
                                static_cast<uint>(columnPos_WS.x), blockY, static_cast<uint>(columnPos_WS.y));
                            const vec2 tangentJitter = (jitterRng.nextFloat2() - .5f) * .4f;
                            jitter = tangentJitter.x * faceTangentX[mountFace] +
                                     tangentJitter.y * faceTangentZ[mountFace];
                        }
                        const vec3 offset = vec3(blockPos_CS) + vec3(.5f) - .5f * mountNormal + jitter;
                        const auto baseVertex = static_cast<uint32_t>(terrainVerts.size());
                        const auto baseTriangle = static_cast<uint32_t>(terrainIdxs.size() / 3);
                        const auto& vertices = model.orientations[mountFace * 4 + turn];
                        terrainVerts.insert(terrainVerts.end(), vertices.begin(), vertices.end());
                        for (size_t i = baseVertex; i < terrainVerts.size(); ++i)
                        {
                            auto& pos = terrainVerts[i].pos_OS;
                            pos.x += offset.x;
                            pos.y += offset.y;
                            pos.z += offset.z;
                        }
                        const size_t baseIndex = terrainIdxs.size();
                        terrainIdxs.insert(terrainIdxs.end(), model.indices.begin(), model.indices.end());
                        for (size_t i = baseIndex; i < terrainIdxs.size(); ++i) terrainIdxs[i] += baseVertex;
                        const auto data = makeBlockTriangleData(blockData, blockData.texSlices[0]);
                        const auto triangleCount = static_cast<uint32_t>(model.indices.size() / 3);
                        terrainPerTriDatas.insert(terrainPerTriDatas.end(), triangleCount, data);
                        // Custom UVs cannot use the full-quad cutout OMM pair. Startup validates opacity.
                        if (useOmms) terrainOmmIdxs.insert(terrainOmmIdxs.end(), triangleCount, TerrainOmm::OMM_IDX_FULLY_OPAQUE);
                        if (blockData.markAsEmitter)
                            for (uint32_t i = 0; i < triangleCount; ++i) terrainEmissiveTriangleIdxs.push_back(baseTriangle + i);
                    }
                    else if (blockData.shape == BlockShape::X_SHAPED)
                    {
                        const uint baseVertIdx = static_cast<uint>(terrainVerts.size());

                        // Jitter is hashed from world XZ so vertically stacked X-shaped blocks stay aligned.
                        const ivec2 columnPos_WS = this->chunkPos * static_cast<int>(chunkSizeXZ) + ivec2(blockX, blockZ);
                        vec2 jitter(0.f);
                        if (blockData.randomJitter)
                        {
                            RandomNumberGenerator jitterRng = initRng(worldSeed ^ hash(392421012),
                                                                      static_cast<uint>(columnPos_WS.x),
                                                                      static_cast<uint>(columnPos_WS.y /*z*/));
                            jitter = (jitterRng.nextFloat2() - 0.5f) * 0.4f;
                        }
                        const vec3 basePos_CS = vec3(blockPos_CS) + vec3(jitter.x, 0, jitter.y /*z*/);

                        const uint32_t texArraySliceIdx = blockData.texSlices[1]; // top; all faces of an X-shaped block share one texture
                        for (uint i = 0; i < 8; ++i)
                        {
                            const vec3 vertPos_CS = basePos_CS + xShapedFaceVertPositions[i];
                            terrainVerts.emplace_back(
                                makeVertex(vertPos_CS, xShapedFaceNormals[i / 4], vec2(uvOffsets[i % 4])));
                        }

                        for (uint j = 0; j < 2; ++j)
                        {
                            const uint offset = j * 4;
                            terrainIdxs.emplace_back(baseVertIdx + offset + 0u);
                            terrainIdxs.emplace_back(baseVertIdx + offset + 1u);
                            terrainIdxs.emplace_back(baseVertIdx + offset + 2u);
                            terrainIdxs.emplace_back(baseVertIdx + offset + 0u);
                            terrainIdxs.emplace_back(baseVertIdx + offset + 2u);
                            terrainIdxs.emplace_back(baseVertIdx + offset + 3u);
                        }

                        terrainPerTriDatas.insert(terrainPerTriDatas.end(), 4,
                                                  makeBlockTriangleData(blockData, texArraySliceIdx));

                        if (useOmms)
                        {
                            appendOmmIdxs(texArraySliceIdx, 4);
                        }
                    }
                    else // BlockShape::LIQUID_TOP or BlockShape::CUBE
                    {
                        const bool isWater = (blockData.type == BlockType::WATER);
                        std::vector<Vertex>& verts = isWater ? waterVerts : terrainVerts;
                        std::vector<uint32_t>& idxs = isWater ? waterIdxs : terrainIdxs;
                        std::vector<PerTriangleData>& perTriDatas = isWater ? waterPerTriDatas : terrainPerTriDatas;
                        const float topYSubtract = (blockData.shape == BlockShape::LIQUID_TOP) ? (1.f / 8.f) : 0.f;

                        for (uint faceIdx = 0; faceIdx < 6; ++faceIdx)
                        {
                            const ivec3 neighborOffset = faceOffsets[faceIdx];
                            const ivec3 neighborPos_CS = ivec3(blockPos_CS) + neighborOffset;

                            if (!shouldGenerateFace(blockPos_CS, blockData.type, blockData.shape, neighborPos_CS, faceIdx))
                            {
                                continue;
                            }

                            const uint baseVertIdx = static_cast<uint>(verts.size());

                            const ivec3* thisFaceVertPositions = cubeFaceVertPositions + (faceIdx * 4);
                            const uint32_t texArraySliceIdx = blockData.texSlices[glm::max(static_cast<int>(faceIdx) - 3, 0)];
                            for (uint i = 0; i < 4; ++i)
                            {
                                vec3 vertPos_CS = vec3(ivec3(blockPos_CS) + thisFaceVertPositions[i]);
                                if (thisFaceVertPositions[i].y == 1)
                                {
                                    vertPos_CS.y -= topYSubtract;
                                }

                                verts.emplace_back(makeVertex(vertPos_CS, vec3(neighborOffset), vec2(uvOffsets[i])));
                            }

                            const uint32_t triangleIdx = static_cast<uint32_t>(idxs.size() / 3u);

                            idxs.emplace_back(baseVertIdx + 0u);
                            idxs.emplace_back(baseVertIdx + 1u);
                            idxs.emplace_back(baseVertIdx + 2u);
                            idxs.emplace_back(baseVertIdx + 0u);
                            idxs.emplace_back(baseVertIdx + 2u);
                            idxs.emplace_back(baseVertIdx + 3u);

                            auto faceData = makeBlockTriangleData(blockData, texArraySliceIdx);
                            if (isWater) faceData.flags |= TRIANGLE_FLAG_IS_WATER;
                            if (isWater && faceIdx == 4) faceData.flags |= TRIANGLE_FLAG_IS_WATER_TOP;
                            perTriDatas.emplace_back(faceData);
                            perTriDatas.emplace_back(faceData);

                            if (useOmms && !isWater)
                            {
                                appendOmmIdxs(texArraySliceIdx, 2);
                            }

                            if (blockData.markAsEmitter)
                            {
                                // water does not emit light so it will never reach this
                                terrainEmissiveTriangleIdxs.emplace_back(triangleIdx);
                                terrainEmissiveTriangleIdxs.emplace_back(triangleIdx + 1u);
                            }
                        }
                    }
                }
            }
        }
    }

    ASSERT(terrainVerts.size() > 0);
    ASSERT(terrainIdxs.size() > 0);

    if (useOmms && !hasCutoutFaces)
    {
        // No cutout faces means no OMM linkage is needed: the whole geometry can be flagged
        // opaque instead, which also skips anyhit entirely
        terrainOmmIdxs.clear();
    }
    this->terrainInstance->setIsOpaque(useOmms && !hasCutoutFaces);

    const ivec2 chunkBlockPos_WS = this->chunkPos * static_cast<int>(chunkSizeXZ);
    const ivec3 transformOffset = ivec3(chunkBlockPos_WS.x, 0, chunkBlockPos_WS.y /*z*/);

    terrainInstance->setTransformOffset(transformOffset);
    terrainInstance->finalizeGeometry();
    terrainInstance->setMaterialIdx(TerrainMaterials::getMaterialIdx(TerrainMaterial::DEFAULT));
    terrainInstance->addAreaLights(terrainEmissiveTriangleIdxs);

    if (!waterVerts.empty())
    {
        waterInstance->setTransformOffset(transformOffset);
        waterInstance->finalizeGeometry();
        waterInstance->setMaterialIdx(TerrainMaterials::getMaterialIdx(TerrainMaterial::WATER));
        waterInstance->setIsDeformable(true);
    }

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

void Chunk::destroyInstances(ToFreeList& toFreeList)
{
    toFreeList.pushInstance(this->terrainInstance);
    this->terrainInstance = nullptr;
    if (this->waterInstance != nullptr)
    {
        toFreeList.pushInstance(this->waterInstance);
        this->waterInstance = nullptr;
    }
    this->setState(ChunkState::NEEDS_GEOMETRY);
    this->setIsMarkedForDestruction(false);
}

void Chunk::cleanUnusedInstances(ToFreeList& toFreeList)
{
    // if the geometry was never finalized, that means the instance has no verts
    if (this->waterInstance != nullptr && !this->waterInstance->getIsGeometryFinalized())
    {
        toFreeList.pushInstance(this->waterInstance);
        this->waterInstance = nullptr;
    }
}

Instance* Chunk::getTerrainInstance() const
{
    return this->terrainInstance;
}

Instance* Chunk::getWaterInstance() const
{
    return this->waterInstance;
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

void Chunk::loadSerializedData(std::vector<Block>&& blocks, std::vector<Biome>&& biomes,
                               std::vector<Structure>&& structures,
                               std::unordered_map<uint32_t, uint8_t>&& blockStates)
{
    ASSERT(blocks.size() == numChunkBlocks);
    ASSERT(biomes.size() == chunkSizeXZSquare);

    this->blocks = std::move(blocks);
    this->biomes = std::move(biomes);
    this->structures = std::move(structures);
    this->blockStates = std::move(blockStates);
    this->wasImported = true;
}

bool Chunk::getIsMarkedForDestruction() const
{
    return this->isMarkedForDestruction.load(std::memory_order_acquire);
}

bool Chunk::getWasImported() const
{
    return this->wasImported;
}

void Chunk::setIsMarkedForDestruction(bool marked)
{
    this->isMarkedForDestruction.store(marked, std::memory_order_release);
}

void Chunk::setInstancesVisible(bool visible)
{
    this->areInstancesVisible = visible;
    if (this->terrainInstance != nullptr) // this check is needed as this function could be called before terrainInstance is set
    {
        this->terrainInstance->setVisible(visible);
    }
    if (this->waterInstance != nullptr)
    {
        this->waterInstance->setVisible(visible);
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

bool Chunk::tryGetBlock(glm::uvec3 chunkBlockPos, Block& outBlock) const
{
    if (this->getState() < ChunkState::HAS_ALL_BLOCKS)
    {
        return false;
    }

    outBlock = this->blocks[Chunk::blockPosToIdx(chunkBlockPos)];
    return true;
}

const std::vector<Block>& Chunk::getBlocks() const
{
    return this->blocks;
}

const std::vector<Biome>& Chunk::getBiomes() const
{
    return this->biomes;
}

const std::vector<Structure>& Chunk::getStructures() const
{
    return this->structures;
}

const std::unordered_map<uint32_t, uint8_t>& Chunk::getBlockStates() const
{
    return this->blockStates;
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
    ASSERT(this->chunks[chunkIdx] == nullptr, "createChunk called on already-populated slot");
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
