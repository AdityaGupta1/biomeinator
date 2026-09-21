// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/common/common_structs.h"

#include "debug.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>

// CPU-side equivalents of the packing functions in shaders/util/packing.hlsli; the math must
// stay bit-identical with the HLSL versions so packed vertex data decodes consistently
namespace Util
{

inline uint32_t packFloat2ToUint(const float x, const float y)
{
    const uint32_t hx = DirectX::PackedVector::XMConvertFloatToHalf(x);
    const uint32_t hy = DirectX::PackedVector::XMConvertFloatToHalf(y);
    return hx | (hy << 16);
}

inline uint32_t packSnorm2ToUint(const float x, const float y)
{
    const int ix = static_cast<int>(std::round(std::clamp(x, -1.f, 1.f) * 32767.f));
    const int iy = static_cast<int>(std::round(std::clamp(y, -1.f, 1.f) * 32767.f));
    return (static_cast<uint32_t>(ix) & 0xFFFF) | (static_cast<uint32_t>(iy) << 16);
}

inline uint32_t octEncode(const DirectX::XMFLOAT3& nor)
{
    const float invL1 = 1.f / (std::abs(nor.x) + std::abs(nor.y) + std::abs(nor.z));
    float nx = nor.x * invL1;
    float ny = nor.y * invL1;
    const float nz = nor.z * invL1;
    if (nz < 0.f)
    {
        const float wrappedX = (1.f - std::abs(ny)) * (nx >= 0.f ? 1.f : -1.f);
        const float wrappedY = (1.f - std::abs(nx)) * (ny >= 0.f ? 1.f : -1.f);
        nx = wrappedX;
        ny = wrappedY;
    }
    return packSnorm2ToUint(nx, ny);
}

inline uint32_t packTerrainPosComponent(const float value, const float bias, const float scale)
{
    const long quantized = std::lround((value + bias) * scale);
    ASSERT(quantized >= 0 && quantized <= 0xFFFF);
    return static_cast<uint32_t>(quantized);
}

inline uint32_t packUnorm8(const float value)
{
    return static_cast<uint32_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

inline PackedTerrainVertex packTerrainVertex(const Vertex& vert)
{
    const uint32_t x = packTerrainPosComponent(vert.pos_OS.x, PACKED_TERRAIN_POS_XZ_BIAS, PACKED_TERRAIN_POS_XZ_SCALE);
    const uint32_t y = packTerrainPosComponent(vert.pos_OS.y, PACKED_TERRAIN_POS_Y_BIAS, PACKED_TERRAIN_POS_Y_SCALE);
    const uint32_t z = packTerrainPosComponent(vert.pos_OS.z, PACKED_TERRAIN_POS_XZ_BIAS, PACKED_TERRAIN_POS_XZ_SCALE);
    return {
        .packedPosXY = x | (y << 16),
        .packedPosZUv = z | (packUnorm8(vert.uv.x) << 16) | (packUnorm8(vert.uv.y) << 24),
        .packedNor = vert.packedNor,
    };
}

} // namespace Util
