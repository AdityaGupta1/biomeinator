// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>

class Scene;

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
// Whether the aux tile has any per-texel mask bit set (see AUX_MASK_* in common_structs.h)
bool sliceHasAuxMasks(uint32_t sliceIdx);

} // namespace TerrainMaterials
