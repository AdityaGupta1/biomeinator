// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>

// Kept free of chunk.h's renderer dependencies so CPU-only code (region files, unit tests) can use them.
inline constexpr uint32_t chunkSizeXZ = 16;
inline constexpr uint32_t chunkSizeXZSquare = chunkSizeXZ * chunkSizeXZ;
inline constexpr uint32_t chunkSizeY = 512;
inline constexpr uint32_t numChunkBlocks = chunkSizeXZSquare * chunkSizeY;

inline constexpr uint32_t regionSideLength = 32;
