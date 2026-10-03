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
// tile generation tasks to enqueue.
void update(glm::ivec2 cameraChunkPos,
            int renderDistance,
            int lodDistance,
            Chunk* (*findChunk)(glm::ivec2 chunkPos),
            ToFreeList& toFreeList,
            std::vector<Task>& outTasks);

// No generation task may be queued or running
void reset(ToFreeList& toFreeList);

} // namespace TerrainLod
