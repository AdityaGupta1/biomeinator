// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

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

} // namespace Util
