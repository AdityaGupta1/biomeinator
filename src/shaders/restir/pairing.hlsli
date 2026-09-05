// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_params.h"

#include "common/global_params.hlsli"

// Screen-space partner of a pixel under pairing texture `textureIdx`, false if it falls off screen.
// The per-frame transform T (transpose, flips, offset) maps a pixel to texture coordinates; the
// partner is p + T^-1(delta), which keeps the screen pairing an involution: T(partner) lands on the
// texture partner's texel, whose delta is the negation.
bool getPairedPixel(StructuredBuffer<uint> pairingTextures, const uint textureIdx, const uint2 pixelIdx, out uint2 partnerIdx)
{
    const uint4 transform = restirParams.pairingTransforms[textureIdx];
    const int size = int(transform.x);
    const uint flags = transform.y;

    int2 texCoord = int2(pixelIdx) + int2(transform.zw);
    if (bool(flags & RESTIR_PAIRING_TRANSPOSE))
    {
        texCoord = texCoord.yx;
    }
    if (bool(flags & RESTIR_PAIRING_FLIP_X))
    {
        texCoord.x = -texCoord.x;
    }
    if (bool(flags & RESTIR_PAIRING_FLIP_Y))
    {
        texCoord.y = -texCoord.y;
    }
    const int2 texel = (texCoord % size + size) % size;

    const uint packed = pairingTextures[restirParams.pairingBufferOffsets[textureIdx] + uint(texel.y * size + texel.x)];
    int2 delta = int2(int(packed << 24) >> 24, int(packed << 16) >> 24); // sign-extend the two int8 channels

    if (bool(flags & RESTIR_PAIRING_FLIP_X))
    {
        delta.x = -delta.x;
    }
    if (bool(flags & RESTIR_PAIRING_FLIP_Y))
    {
        delta.y = -delta.y;
    }
    if (bool(flags & RESTIR_PAIRING_TRANSPOSE))
    {
        delta = delta.yx;
    }

    const int2 partner = int2(pixelIdx) + delta;
    partnerIdx = uint2(partner);
    return all(partner >= 0) && all(partner < int2(renderParams.renderSize));
}
