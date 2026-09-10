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
    std::array<std::vector<Vertex>, 24> orientations;
    std::vector<uint32_t> indices;
    bool hasAllFaceOrientations{ false };

    const std::vector<Vertex>& getOrientation(uint8_t face, uint8_t turn) const;
};

// Static, uncompressed GLB geometry only. Materials/textures belong to the block.
Model readGlb(const std::filesystem::path& path, bool allFaces = true);
void clear();
uint32_t load(const std::filesystem::path& path, bool allFaces);
const Model& get(uint32_t id);
} // namespace BlockModels
