// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <glm/glm.hpp>

#include <vector>

class Chunk;
class Scene;
class ToFreeList;
struct Task;

// Distant terrain as heightfield tiles in a quadtree, refined toward the camera down to full-resolution
// chunks. See knowledge/terrain/terrain_lod.md.
namespace TerrainLod
{

void init(Scene* scene);

// Main thread. Decides which tiles and chunks are shown, owning chunk visibility, and appends the
// tile generation tasks to enqueue. Chunks within chunkDistance replace tiles once they are ready, and
// tiles within voxelDistance (0 for none) are downsampled blocks rather than heightfields.
void update(glm::ivec2 cameraChunkPos,
            int chunkDistance,
            int voxelDistance,
            int lodDistance,
            Chunk* (*findChunk)(glm::ivec2 chunkPos),
            ToFreeList& toFreeList,
            std::vector<Task>& outTasks);

// No generation task may be queued or running
void reset(ToFreeList& toFreeList);

// Whether the last update showed the chunk
bool isChunkDisplayed(glm::ivec2 chunkPos);

// Inclusive chunk bounds of the area the tiles cover
void getCoveredChunkBounds(glm::ivec2 cameraChunkPos, int lodDistance, glm::ivec2& outMinChunkPos,
                           glm::ivec2& outMaxChunkPos);

} // namespace TerrainLod
