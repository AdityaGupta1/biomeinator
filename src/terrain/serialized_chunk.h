// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "biome.h"
#include "block.h"
#include "chunk_dimensions.h"
#include "structure/cave_structure.h"
#include "structure/structure.h"

#include <glm/vec2.hpp>
#include <span>
#include <unordered_map>
#include <vector>

// Completed block generation only. Empty terrain masks and heights are permitted solely for
// legacy imports.
struct SerializedChunkData
{
    std::vector<Block> blocks;
    std::vector<Biome> biomes;
    std::vector<Structure> structures;
    std::unordered_map<uint32_t, uint8_t> blockStates;
    std::vector<CaveStructure> caveStructures;
    std::vector<SurfaceStructureCandidate> surfaceStructureCandidates;
    std::vector<uint64_t> terrainAirMask;
    std::vector<uint64_t> terrainSolidCubeMask;
    std::vector<uint16_t> terrainTopY;
    std::vector<uint16_t> terrainSurfaceHeight;
};

// Borrows a completed chunk's data for writing; the owner must outlive the view.
struct SerializedChunkView
{
    glm::ivec2 position;
    std::span<const Block> blocks;
    std::span<const Biome> biomes;
    std::span<const Structure> structures;
    const std::unordered_map<uint32_t, uint8_t>* blockStates;
    std::span<const CaveStructure> caveStructures;
    std::span<const SurfaceStructureCandidate> surfaceStructureCandidates;
    std::span<const uint64_t> terrainAirMask;
    std::span<const uint64_t> terrainSolidCubeMask;
    std::span<const uint16_t> terrainTopY;
    std::span<const uint16_t> terrainSurfaceHeight;
};
