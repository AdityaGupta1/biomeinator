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

#define CHUNK_SIZE_XZ 16
#define CHUNK_SIZE_Y 256

class Region;

class Chunk
{
private:
    const glm::ivec2 chunkPos;
    const Region* region;

    std::atomic<ChunkState> state{ ChunkState::NEEDS_BLOCKS };
    bool isMarkedForDestruction{ false };

    std::array<Block, CHUNK_SIZE_XZ * CHUNK_SIZE_Y * CHUNK_SIZE_XZ> blocks{};

    Instance* instance{ nullptr };

    bool isBlockAir(glm::ivec3 pos_CS);

public:
    Chunk(glm::ivec2 chunkPos, Region* region);

    void generateBlocks();

    void createInstance(Scene* scene, Instance* instance);

    void destroyInstance(ToFreeList& toFreeList);

    Instance* getInstance() const;

    ChunkState getState() const;

    void setState(ChunkState newState);

    void setMarkedForDestruction(bool mark = true);

    static uint32_t blockPosToIdx(glm::uvec3 chunkBlockPos);
};

#define REGION_SIDE_LENGTH 16

class Region
{
private:
    std::array<Region*, 4> neighbors{};

public:
    const glm::ivec2 regionPos;
    const glm::ivec2 regionPosChunks;

    std::array<std::unique_ptr<Chunk>, REGION_SIDE_LENGTH * REGION_SIDE_LENGTH> chunks{};

    Region(glm::ivec2 regionPos);

    Chunk* operator[](glm::ivec2 chunkPos);
    Chunk* getOrCreateChunk(glm::ivec2 chunkPos);

    void setNeighbor(NeighborDirection dir, Region* neighborRegion);

    static uint32_t chunkPosToIdx(glm::ivec2 regionChunkPos);
};
