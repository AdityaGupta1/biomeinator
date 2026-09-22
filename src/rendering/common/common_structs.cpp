// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "common_structs.h"

#include "debug.h"

Material::Material()
    : flags(MATERIAL_FLAG_DIFFUSE),
      diffuseTransmission(0.f),
      roughness(0.f),
      baseColor{ 1, 1, 1 },
      baseColorTextureId(TEXTURE_ID_INVALID),
      glossyReflectionTint{ 1, 1, 1 },
      ior(1.5f),
      emissiveStrength(0),
      emissiveColor{ 1, 1, 1 },
      auxTextureId(TEXTURE_ID_INVALID),
      normalTextureId(TEXTURE_ID_INVALID),
      roughnessTextureId(TEXTURE_ID_INVALID),
      normalScale(1.f)
{}

PerFaceData::PerFaceData()
    : packedFlagsAndSlice(0),
      localAreaLightIdx(LIGHT_IDX_INVALID)
{}

void PerFaceData::setFlags(const uint32_t flags)
{
    ASSERT((flags & ~FACE_FLAGS_MASK) == 0);
    this->packedFlagsAndSlice = (this->packedFlagsAndSlice & ~FACE_FLAGS_MASK) | flags;
}

void PerFaceData::setTexArraySliceIdx(const uint32_t sliceIdx)
{
    ASSERT(sliceIdx < (1u << (32 - FACE_FLAGS_BITS)));
    this->packedFlagsAndSlice = (this->packedFlagsAndSlice & FACE_FLAGS_MASK) | (sliceIdx << FACE_FLAGS_BITS);
}
