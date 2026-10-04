// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "block_ids.h"
#include "chunk_dimensions.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

struct BlockData;
struct Task;

// Voxel cells are this many blocks on a side
inline constexpr int voxelCellSize = 2;
inline constexpr int cellsPerChunkSide = static_cast<int>(chunkSizeXZ) / voxelCellSize;
// Heights in a cell are in eighths of a block, the granularity of block shape heights
inline constexpr int fillUnitsPerBlock = 8;
inline constexpr int fullCellFill = voxelCellSize * fillUnitsPerBlock;

// A downsampled cell: filling the cell from its bottom up to fill, with its block's sides and the top of
// the block on top, as a snow layer on grass shows snow above the grass's sides
struct VoxelCell
{
    Block block{ Block::AIR };
    Block topBlock{ Block::AIR };
    uint8_t fill{ 0 };
    // Height of the cell's highest water above its bottom, 0 for none
    uint8_t waterFill{ 0 };

    bool isFull() const
    {
        return this->fill == fullCellFill;
    }
};

// A surface-only chunk downsampled into cells, over the band of heights where its surface lies. Below the
// band is solid rock, above it air.
struct SurfaceChunkCells
{
    // Block height of the band's bottom, a multiple of voxelCellSize
    int minBlockY{ 0 };
    int numCellsY{ 0 };
    // Indexed x + cellsPerChunkSide * (z + cellsPerChunkSide * y)
    std::vector<VoxelCell> cells;
    // Biome tint per cell column
    std::array<uint32_t, cellsPerChunkSide * cellsPerChunkSide> packedTints{};
    // The real blocks along each side (+X, +Z, -X, -Z, as block faces), over the band: tiles cull the faces
    // beside the chunk against them rather than against its cells, which cover more. Indexed by position
    // along the side + chunkSizeXZ * (y - minBlockY).
    std::array<std::vector<Block>, 4> sideBlocks;

    int getMaxBlockY() const
    {
        return this->minBlockY + this->numCellsY * voxelCellSize;
    }

    // y in cells from the world's bottom
    VoxelCell cellAt(glm::ivec2 cellXZ, int cellY) const;
    Block sideBlockAt(int sideIdx, int alongIdx, int blockY) const;
};

// Blocks that fill their cell from the bottom up to their shape's top height, which are the ones cells
// can show. Plants and models are too small to.
bool fillsFromBottom(const BlockData& blockData);
// The shape's top height in fill units
int blockFill(const BlockData& blockData);

class SurfaceChunk;

// Generates surface-only chunks for voxel LOD tiles, a task per chunk and each once, and caches them
// downsampled. See knowledge/terrain/terrain_lod.md.
namespace SurfaceChunkCache
{

// Main thread. The chunk's cells if they are ready; otherwise requests them, at priority (lower first),
// and returns null.
const SurfaceChunkCells* requestCells(glm::ivec2 chunkPos, float priority);

// Main thread. Keeps the chunk's cells while a tile meshes from them.
void pinCells(glm::ivec2 chunkPos);
void unpinCells(glm::ivec2 chunkPos);

// Main thread, after this frame's requests. Starts generation for the requests, highest priority first,
// appending the tasks to enqueue, and drops what is no longer needed. Cells are kept, even unrequested,
// within keepDistance chunks of the camera and a margin past it: tiles need them again when the camera
// passes and the chunks leave the chunk distance behind it, or when it turns back.
void update(glm::ivec2 cameraChunkPos, int keepDistance, std::vector<Task>& outTasks);

// No generation task may be queued or running
void reset();

} // namespace SurfaceChunkCache
