// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "rendering/common/common_structs.h"

#include <cstdint>

class Scene;
struct BlockData;

enum class TerrainMaterial : uint8_t
{
	DEFAULT,
	WATER,

	COUNT
};

namespace TerrainMaterials
{

void init(Scene* scene);

uint32_t getMaterialIdx(TerrainMaterial terrainMaterial);

// Whether the aux map tile for this texture array slice has any biome tint mask coverage
bool sliceHasBiomeTint(uint32_t sliceIdx);
bool sliceHasNormalMap(uint32_t sliceIdx);

// Per-face data of a block face showing the given texture array slice
PerFaceData makeBlockFaceData(const BlockData& block, uint32_t slice, uint32_t extraFlags = 0);

} // namespace TerrainMaterials
