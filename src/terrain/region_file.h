// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "chunk.h"

#include <glm/vec2.hpp>
#include <optional>
#include <span>

namespace RegionFile
{

struct DecodedChunk
{
    glm::ivec2 position;
    SerializedChunkData data;
};

using DecodedRegion = std::vector<DecodedChunk>;

std::string fileName(glm::ivec2 regionPos);
bool isValidPosition(glm::ivec2 regionPos);

// The caller retains these completed chunks for the duration of the write.
// An empty region is valid. Block values index the current Blocks::blockIdNames palette.
bool write(const std::filesystem::path& path, glm::ivec2 position, std::span<const Chunk* const> chunks);

// The result has no live chunk/region pointers. Attach on the main thread only
// after reserving the destination chunks against generation and other readers.
std::optional<DecodedRegion> read(const std::filesystem::path& path, glm::ivec2 expectedPos,
                                  std::span<const Block> blockRemap);

} // namespace RegionFile
