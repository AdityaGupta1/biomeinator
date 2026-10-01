// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "serialized_chunk.h"

#include <filesystem>
#include <glm/vec2.hpp>
#include <optional>
#include <span>
#include <string>

namespace RegionFile
{

struct DecodedChunk
{
    glm::ivec2 position;
    SerializedChunkData data;
};

using DecodedRegion = std::vector<DecodedChunk>;

// Engine registries the codec validates against. Passed in so the codec does not depend on
// loaded block assets or biome configuration.
struct Registry
{
    std::span<const BlockStateKind> blockStateKinds; // indexed by Block
    const SurfaceStructureGens* surfaceStructureGens;
};

std::string fileName(glm::ivec2 regionPos);

// The views' chunks must have all their blocks and outlive the write.
// An empty region is valid. Block values index the current Blocks::blockIdNames palette.
bool write(const std::filesystem::path& path, glm::ivec2 position, std::span<const SerializedChunkView> chunks,
           const Registry& registry);

// The result has no live chunk/region pointers. Attach on the main thread only
// after reserving the destination chunks against generation and other readers.
std::optional<DecodedRegion> read(const std::filesystem::path& path, glm::ivec2 expectedPos,
                                  std::span<const Block> blockRemap, const Registry& registry);

} // namespace RegionFile
