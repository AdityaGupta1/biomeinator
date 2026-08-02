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

} // namespace TerrainMaterials
