// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_common.h"

#include <cstdint>
#include <vector>

class ToFreeList;

// Bakes opacity micromaps for the terrain's alpha-cutout texture array slices and builds the
// shared OMM Array referenced by all terrain BLASes. Each cutout slice gets two 2-state OMMs
// (one per quad triangle) that reproduce the point-sampled mip-0 alpha test exactly.
//
// NOTE: OMMs have no LOD, so distant foliage bypasses the coverage-preserving alpha mips and
// reads thinner than the anyhit path rendered it; see the bad-interaction section in
// knowledge/terrain/terrain_omm.md before changing distance behavior.
namespace TerrainOmm
{

// R16 encoding of the special index (-2), which marks a triangle as having no OMM
inline constexpr uint16_t OMM_IDX_FULLY_OPAQUE =
    static_cast<uint16_t>(D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE);

// mip0Alpha is one byte per texel of the full texture atlas (textureSize x textureSize),
// split into (textureSize / tileSize)^2 slices indexed as tileY * tilesPerAxis + tileX
void bake(const std::vector<uint8_t>& mip0Alpha, uint32_t textureSize, uint32_t tileSize);

bool isBaked();

bool texArraySliceHasCutout(uint32_t texArraySliceIdx);

// OMM Array entry for one triangle of a cutout slice's quad (triInQuad 0 = (0,1,2), 1 = (0,2,3))
uint16_t getOmmIdx(uint32_t texArraySliceIdx, uint32_t triInQuad);

// Builds the OMM Array on the first call after bake(); no-op on later calls
void buildArrayIfPending(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

// Frees the OMM Array section; must run before AcsHelper::reset()
void reset();

} // namespace TerrainOmm
