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
    std::array<std::vector<Vertex>, 4> rotations;
    std::vector<uint32_t> indices;
};

// Static, uncompressed GLB geometry only. Materials/textures belong to the block.
Model readGlb(const std::filesystem::path& path);
void clear();
uint32_t load(const std::filesystem::path& path);
const Model& get(uint32_t id);
} // namespace BlockModels
