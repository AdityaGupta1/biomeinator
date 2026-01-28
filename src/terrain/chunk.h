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

#pragma once

#include "block.h"
#include "scene/scene.h"

#include <array>
#include <atomic>
#include <glm/glm.hpp>

enum class ChunkState : uint8_t
{
    NEEDS_BLOCKS,
    GENERATING_BLOCKS,
    HAS_BLOCKS,
    NEEDS_SEGMENTS,
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
    BLOCKS_SURROUNDED,
    MIXED,
};

inline constexpr uint32_t chunkSizeXZ = 16;
inline constexpr uint32_t chunkSizeXZSquare = chunkSizeXZ * chunkSizeXZ;
inline constexpr uint32_t chunkSizeY = 384;
inline constexpr uint32_t numChunkBlocks = chunkSizeXZSquare * chunkSizeY;

inline constexpr uint32_t chunkSegmentSizeXZ = 4;
inline constexpr uint32_t chunkSegmentSizeY = 8;

static_assert(chunkSizeXZ % chunkSegmentSizeXZ == 0);
static_assert(chunkSizeY % chunkSegmentSizeY == 0);

inline constexpr uint32_t numChunkSegmentsXZ = chunkSizeXZ / chunkSegmentSizeXZ;
inline constexpr uint32_t numChunkSegmentsY = chunkSizeY / chunkSegmentSizeY;
inline constexpr uint32_t numChunkSegments = numChunkSegmentsXZ * numChunkSegmentsY * numChunkSegmentsXZ;

class Region;

class Chunk
{
private:
    const glm::ivec2 chunkPos;
    Region* const region;

    std::vector<Block> blocks{};
    std::vector<ChunkSegment> allSegments{};
    std::vector<glm::uvec3> segmentsToGenerate{};

    std::array<Chunk*, 4> neighbors{};
    uint32_t numNeighborsSet{ 0 };
    std::atomic<uint32_t> numNeighborsWithBlocks{ 0 };

    std::atomic<ChunkState> state{ ChunkState::NEEDS_BLOCKS };
    std::atomic<bool> isMarkedForDestruction{ false };
    bool isInstanceVisible{ false };

    Instance* instance{ nullptr };

    bool isBlockAir(glm::ivec3 pos_CS, int faceIdx);

    bool isRegionAirOrSolid(const glm::uvec3 startPos, const glm::uvec3 endPos, bool isAirPredicate);
    bool isSegmentSurroundedBySolid(const glm::uvec3 startPos, const glm::uvec3 endPos, const glm::uvec3 chunkSegmentPos);

    void setNeighbor(NeighborDirection dir, Chunk* neighborChunk);

public:
    Chunk(glm::ivec2 chunkPos, Region* region);

    void setNeighbors(bool createNeighbors);

    void generateBlocks();
    void generateSegments();

    void setInstance(Instance* instance);
    void createInstance();
    void destroyInstance(ToFreeList& toFreeList);
    Instance* getInstance() const;

    ChunkState getState() const;
    void setState(ChunkState newState);
    bool advanceState(ChunkState newState);

    bool getIsMarkedForDestruction();
    void setIsMarkedForDestruction(bool marked = true);

    bool getIsInstanceVisible() const;
    void setInstanceVisible(bool visible);

    glm::ivec2 getChunkPos() const;

    uint32_t getNumNeighborsSet() const;

    static uint32_t blockPosToIdx(glm::uvec3 chunkBlockPos);
    static uint32_t blockPosXZToIdx(glm::uvec2 chunkBlockPos);

    static uint32_t segmentPosToIdx(glm::uvec3 chunkSegmentPos);

    static void segmentPosToBounds(glm::uvec3 chunkSegmentPos, glm::uvec3& outSegmentStartPos, glm::uvec3& outSegmentEndPos);
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
