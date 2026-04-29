// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <glm/glm.hpp>

class Chunk;
class Scene;
class ToFreeList;

namespace Terrain
{

void init(Scene* scene);

void addChunkToCreateBlas(Chunk* chunk);
void addChunkToDestroy(Chunk* chunk);

void setDirty();

void update(ToFreeList& toFreeList);

bool isCameraUnderwater();

void exportWorld();
void importWorld();
bool isWorldFullyLoaded();

glm::ivec3 getVoxelRenderBoundsMin_WS();
glm::ivec3 getVoxelRenderBoundsMax_WS();

void shutdown();

} // namespace Terrain
