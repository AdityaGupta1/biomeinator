// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "biome.h"
#include "block.h"
#include "scene/scene.h"
#include "structure/structure.h"
#include "util/math.h"

#include <array>
#include <atomic>
#include <glm/glm.hpp>

enum class ChunkState : uint8_t
{
    NEEDS_TERRAIN,
    GENERATING_TERRAIN,
    HAS_TERRAIN,
    AWAITING_STRUCTURE_NEIGHBORS,
    NEEDS_FILL_STRUCTURES, // chunks in structureMaxChunkRadius all have structures (>= HAS_TERRAIN)
    FILLING_STRUCTURES,
    HAS_ALL_BLOCKS,
    NEEDS_SEGMENTS, // neighbor chunks all have blocks (>= HAS_ALL_BLOCKS)
    GENERATING_SEGMENTS,
    NEEDS_GEOMETRY,
    GENERATING_GEOMETRY,
    HAS_GEOMETRY,
};

enum class NeighborDirection : uint8_t
{
    X_POS = 0,
    Z_POS = 1,
    X_NEG = 2,
    Z_NEG = 3,
};

constexpr glm::ivec2 neighborOffset(NeighborDirection dir)
{
    switch (dir)
    {
        case NeighborDirection::X_POS:
            return { 1, 0 };
        case NeighborDirection::Z_POS:
            return { 0, 1};
        case NeighborDirection::X_NEG:
            return { -1, 0 };
        case NeighborDirection::Z_NEG:
            return { 0, -1 };
    }

    return { 0, 0 };
}

constexpr NeighborDirection oppositeNeighborDirection(NeighborDirection dir)
{
    return static_cast<NeighborDirection>((static_cast<uint8_t>(dir) + 2) & 0x3);
}

enum class ChunkSegment : uint8_t
{
    AIR,
    SOLID_SURROUNDED,
    MIXED,
};

inline constexpr uint32_t chunkSizeXZ = 16;
inline constexpr uint32_t chunkSizeXZSquare = chunkSizeXZ * chunkSizeXZ;
inline constexpr uint32_t chunkSizeY = 512;
inline constexpr uint32_t numChunkBlocks = chunkSizeXZSquare * chunkSizeY;
inline constexpr glm::ivec3 chunkSizeVec = { chunkSizeXZ, chunkSizeY, chunkSizeXZ };

static_assert(MathUtil::isPowerOfTwo(chunkSizeXZ), "chunkSizeXZ must be a power of two");

inline constexpr uint32_t chunkSegmentSizeXZ = 4;
inline constexpr uint32_t chunkSegmentSizeY = 8;

static_assert(chunkSizeXZ % chunkSegmentSizeXZ == 0, "chunkSizeXZ must be a multiple of chunkSegmentSizeXZ");
static_assert(chunkSizeY % chunkSegmentSizeY == 0, "chunkSizeY must be a multiple of chunkSegmentSizeY");

inline constexpr uint32_t numChunkSegmentsXZ = chunkSizeXZ / chunkSegmentSizeXZ;
inline constexpr uint32_t numChunkSegmentsY = chunkSizeY / chunkSegmentSizeY;
inline constexpr uint32_t numChunkSegments = numChunkSegmentsXZ * numChunkSegmentsY * numChunkSegmentsXZ;

class Region;
class ThreadMemoryAllocator;

class Chunk
{
private:
    const glm::ivec2 chunkPos;
    Region* const region;

    std::vector<Block> blocks{};
    std::vector<glm::uvec3> segmentsToGenerate{};

    std::vector<Biome> biomes{};

    std::vector<Structure> structures{};
    std::vector<const Chunk*> structureNeighbors{};
    std::atomic<uint32_t> numReadyStructureNeighbors{ 0 };

    std::array<Chunk*, 4> neighbors{};
    uint32_t numNeighborsSet{ 0 };
    std::atomic<uint32_t> numNeighborsWithBlocks{ 0 };

    bool wasImported{ false };

    std::atomic<ChunkState> state{ ChunkState::NEEDS_TERRAIN };
    std::atomic<bool> isMarkedForDestruction{ false };
    bool areInstancesVisible{ false };

    Instance* terrainInstance{ nullptr };
    Instance* waterInstance{ nullptr };

    void fillTerrainBlocksAndCreateStructures(ThreadMemoryAllocator& threadMemoryAlloc);
    void fillStructureBlocks(const Structure* structures, uint32_t numStructures);
    void runStructuresAndDecoratorPass();

    bool shouldGenerateFace(glm::ivec3 thisPos_CS, BlockType thisBlockType, BlockShape thisBlockShape, glm::ivec3 neighborPos_CS, int faceIdx);

    bool isRegionAllBlockType(const glm::uvec3 startPos, const glm::uvec3 endPos, BlockType blockType, BlockShape blockShape = BlockShape::COUNT);
    bool isSegmentSurroundedBySolid(const glm::uvec3 startPos,
                                    const glm::uvec3 endPos,
                                    const glm::uvec3 chunkSegmentPos,
                                    const ChunkSegment* const prevSegments);

    void setNeighbor(NeighborDirection dir, Chunk* neighborChunk);

public:
    Chunk(glm::ivec2 chunkPos, Region* region);

    void setNeighbors(bool createNeighbors);

    void generateTerrain(ThreadMemoryAllocator& threadMemoryAlloc);
    void checkStructureNeighbors();
    void fillStructuresAndDecorators();
    void generateSegments(ThreadMemoryAllocator& threadMemoryAlloc);

    void setInstances(Instance* terrainInstance, Instance* waterInstance);
    void createInstances();
    void destroyInstances(ToFreeList& toFreeList);
    void cleanUnusedInstances(ToFreeList& toFreeList);
    Instance* getTerrainInstance() const;
    Instance* getWaterInstance() const;

    ChunkState getState() const;
    void setState(ChunkState newState);
    bool advanceState(ChunkState newState);

    bool getIsMarkedForDestruction() const;
    void setIsMarkedForDestruction(bool marked = true);

    bool getWasImported() const;

    void setInstancesVisible(bool visible);

    glm::ivec2 getChunkPos() const;

    uint32_t getNumNeighborsSet() const;

    bool tryGetBlock(glm::uvec3 chunkBlockPos, Block& outBlock) const;

    const std::vector<Block>& getBlocks() const;
    const std::vector<Biome>& getBiomes() const;
    const std::vector<Structure>& getStructures() const;

    void loadSerializedData(std::vector<Block>&& blocks, std::vector<Biome>&& biomes, std::vector<Structure>&& structures);

    static uint32_t blockPosToIdx(glm::uvec3 chunkBlockPos);
    static uint32_t blockPosXZToIdx(glm::uvec2 chunkBlockPos);

    static uint32_t segmentPosToIdx(glm::uvec3 chunkSegmentPos);

    static void segmentPosToBounds(glm::uvec3 chunkSegmentPos, glm::uvec3& outSegmentStartPos, glm::uvec3& outSegmentEndPos);

    static inline bool isInChunkXZ(glm::ivec2 pos_CS)
    {
        return glm::min(pos_CS.x, pos_CS.y /*z*/) >= 0 && glm::max(pos_CS.x, pos_CS.y /*z*/) < chunkSizeXZ;
    }

    static inline bool isInChunkXZ(glm::ivec3 pos_CS)
    {
        return isInChunkXZ(glm::ivec2(pos_CS.x, pos_CS.z));
    }

    static inline bool isInChunk(glm::ivec3 pos_CS)
    {
        return isInChunkXZ(pos_CS) && pos_CS.y >= 0 && pos_CS.y < chunkSizeY;
    }
};

inline constexpr uint32_t regionSideLength = 32;

class Region
{
private:
    std::array<Region*, 4> neighbors{};
    uint32_t numNeighborsSet{ 0 };

public:
    const glm::ivec2 regionPos;
    const glm::ivec2 regionPosChunks;

    std::array<std::unique_ptr<Chunk>, regionSideLength * regionSideLength> chunks{};

    Region(glm::ivec2 regionPos);

    Chunk* getChunk(glm::ivec2 chunkPos);
    Chunk* createChunk(glm::ivec2 chunkPos);
    Chunk* getOrCreateChunk(glm::ivec2 chunkPos);

    Region* getNeighbor(NeighborDirection dir) const;
    void setNeighbor(NeighborDirection dir, Region* neighborRegion);
    uint32_t getNumNeighborsSet() const;

    static uint32_t chunkPosToIdx(glm::ivec2 regionChunkPos);
};
