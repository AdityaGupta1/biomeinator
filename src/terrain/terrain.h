// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <filesystem>

#include <glm/glm.hpp>

class Chunk;
class Scene;
class ToFreeList;
enum class Biome : uint8_t;

namespace Terrain
{

void init(Scene* scene);

void addChunkToCreateBlas(Chunk* chunk);
void addChunkToDestroy(Chunk* chunk);

void setDirty();

void update(ToFreeList& toFreeList);

bool isCameraUnderwater();

// Biome of the camera's column from the loaded chunk's per-column biomes (jittered, exactly what
// generated). False while the camera's chunk isn't loaded yet.
bool tryGetCameraBiome(Biome& outBiome);

void exportWorld();
void importWorld();
void reimportWorld(const std::filesystem::path& worldDir);
// True once every imported chunk is queued for its BLAS (or no import is running)
bool pollWorldImport();

glm::ivec3 getVoxelRenderBoundsMin_WS();
glm::ivec3 getVoxelRenderBoundsMax_WS();

void shutdown();

} // namespace Terrain
