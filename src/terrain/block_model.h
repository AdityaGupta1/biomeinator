// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <vector>

#include "rendering/common/common_structs.h"

namespace BlockModels
{
inline constexpr uint32_t INVALID = ~0u;

// Immutable after Blocks::init; all workers share the CPU-side templates.
struct Model
{
    // Surface-mount models use all 24 slots; floor-only models populate only slots 0-3.
    // Four vertices per face, in the implicit quad topology of terrain faces (see TriangleQuads).
    std::array<std::vector<Vertex>, 24> orientations;
    // Faces from here on are lone triangles, whose second triangle is degenerate
    uint32_t numPairedFaces{ 0 };
    bool hasAllFaceOrientations{ false };

    const std::vector<Vertex>& getOrientation(uint8_t face, uint8_t turn) const;
    uint32_t getNumFaces() const;
};

// Static, uncompressed GLB geometry only. Materials/textures belong to the block.
Model readGlb(const std::filesystem::path& path, bool allFaces = true);
void clear();
uint32_t load(const std::filesystem::path& path, bool allFaces);
const Model& get(uint32_t id);
} // namespace BlockModels
