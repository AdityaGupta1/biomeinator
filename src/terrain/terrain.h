// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <filesystem>

#include <glm/glm.hpp>

class Chunk;
class Scene;
class ToFreeList;
enum class Biome : uint8_t;

// Heap bytes held by chunks, by what they are for; see Chunk::getMemory
struct ChunkMemory
{
    uint64_t blocks{ 0 };
    uint64_t terrainMasks{ 0 };
    // Only needed until the structure pass, so mostly held by chunks at the edge of the work zone
    uint64_t generationScratch{ 0 };
    uint64_t structures{ 0 };
    uint64_t misc{ 0 };

    ChunkMemory& operator+=(const ChunkMemory& other);
};

namespace Terrain
{

void init(Scene* scene);

// For createInstances when the chunk's mesh is complete; the main thread then advances it to
// HAS_GEOMETRY
void addChunkWithNewGeometry(Chunk* chunk);
// For workers that advanced a chunk's state: the main thread schedules its next stage without
// rescanning every chunk in range
void addChunkToRevisit(Chunk* chunk);

// Forces a full scan of every chunk in range on the next update
void setDirty();

// With --validateEviction, compares a chunk that just got its blocks against what it held before
// its region was evicted, if it was; see knowledge/terrain/region_system.md
void validateRegeneratedChunk(const Chunk* chunk);

void update(ToFreeList& toFreeList);

// For perf runs measuring world streaming; see knowledge/tests/perf_runs.md
struct StreamingStats
{
    uint32_t numWorkers;
    uint64_t workerBusyNanos;
    uint32_t taskBacklog; // tasks the per-frame cap held back from the pool this frame
    uint32_t tasksPending; // in the pool, queued or executing
};
StreamingStats getStreamingStats();

struct ResidencyStats
{
    uint32_t numRegions;
    uint32_t numChunks;
    ChunkMemory chunkMemory;
    uint64_t pooledChunkBufferBytes;
};
ResidencyStats getResidencyStats();

bool isCameraUnderwater();

// Biome of the camera's column from the loaded chunk's per-column biomes (jittered, exactly what
// generated). False while the camera's chunk isn't loaded yet.
bool tryGetCameraBiome(Biome& outBiome);

void exportWorld();
void importWorld();
void reimportWorld(const std::filesystem::path& worldDir);
bool pollAutomatedRunTerrain();

glm::ivec3 getVoxelRenderBoundsMin_WS();
glm::ivec3 getVoxelRenderBoundsMax_WS();

void shutdown();

} // namespace Terrain
