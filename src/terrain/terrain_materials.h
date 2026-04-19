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

} // namespace TerrainMaterials
