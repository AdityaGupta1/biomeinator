// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "common_structs.h"

Material::Material()
    : flags(MATERIAL_FLAG_DIFFUSE),
      baseColor{ 1, 1, 1 },
      baseColorTextureId(TEXTURE_ID_INVALID),
      glossyReflectionTint{ 1, 1, 1 },
      ior(1.5f),
      emissiveStrength(0),
      emissiveColor{ 1, 1, 1 },
      emissiveColorTextureId(TEXTURE_ID_INVALID)
{}

PerTriangleData::PerTriangleData()
    : flags(0),
      localAreaLightIdx(LIGHT_IDX_INVALID)
{}
