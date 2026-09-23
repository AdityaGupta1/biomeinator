// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "block.h"

#include <glm/vec2.hpp>
#include <memory>
#include <span>

class Region;

namespace RegionFile
{

std::string fileName(glm::ivec2 regionPos);
bool isValidPosition(glm::ivec2 regionPos);

// The caller owns the region lifetime and must not change its chunk slots during
// the call. Only chunks whose blocks are complete are saved; those inputs are immutable.
// An empty region is a valid file. Block values index the current Blocks::blockIdNames palette.
bool write(const std::filesystem::path& path, const Region& region, uint32_t& outChunksWritten);

// Decode privately: failure returns nullptr, never a partially attached region.
// Does not wire neighbors, run tasks, or change world settings/camera.
std::unique_ptr<Region> read(const std::filesystem::path& path, glm::ivec2 expectedPos,
                             std::span<const Block> blockRemap);

} // namespace RegionFile
