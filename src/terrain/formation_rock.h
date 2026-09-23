// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "block.h"

#include <algorithm>
#include <array>

// Tianzi's sandstone family, shared by the formation materials, ledge soil and the pines' ground
// blocks so a new rock needs adding in one place.
namespace FormationRock
{

// Strata alternate these; adjacent layers always differ.
inline constexpr std::array tianziLayerBlocks{ Block::GRAY_SANDSTONE, Block::BUFF_SANDSTONE,
                                               Block::WEATHERED_SANDSTONE };
// Darker patches that cross the strata.
inline constexpr Block tianziPatchBlock = Block::DARK_SANDSTONE;

inline bool isTianziRock(Block block)
{
    return block == tianziPatchBlock || std::ranges::find(tianziLayerBlocks, block) != tianziLayerBlocks.end();
}

} // namespace FormationRock
