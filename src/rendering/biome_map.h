// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"

#include <glm/glm.hpp>

class ToFreeList;

// World-XZ grass tint map: a low-res sRGB texture of per-biome colors covering the render
// distance around the camera, refilled from biome noise whenever the camera crosses a texel
// boundary. Sampled by shaders for faces flagged TRIANGLE_FLAG_BIOME_TINT.
namespace BiomeMap
{

void update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

uint32_t getSrvIdx();
glm::ivec2 getOriginBlocksXZ_WS();
uint32_t getTexelsPerSide();

void destroy();

} // namespace BiomeMap
