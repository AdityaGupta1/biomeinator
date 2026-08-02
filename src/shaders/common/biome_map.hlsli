// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"

SamplerState biomeMapSampler : REGISTER_S(RT, BIOME_MAP_SAMPLER);

// Bicubic B-spline sampling as two offset hardware bilinear taps per axis, so biome borders
// blend C2-smooth over ~2 texels instead of showing bilinear's diamond pattern.
float3 sampleBiomeMapBicubic(const float2 uv)
{
    const float numTexels = sceneParams.biomeMapTexelsPerSide;
    const float2 t = uv * numTexels - 0.5f;
    const float2 i = floor(t);
    const float2 f = t - i;

    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = (1.f / 6.f) * (-f3 + 3.f * f2 - 3.f * f + 1.f);
    const float2 w1 = (1.f / 6.f) * (3.f * f3 - 6.f * f2 + 4.f);
    const float2 w2 = (1.f / 6.f) * (-3.f * f3 + 3.f * f2 + 3.f * f + 1.f);
    const float2 w3 = (1.f / 6.f) * f3;

    const float2 g0 = w0 + w1;
    const float2 g1 = w2 + w3;
    const float2 uv0 = (i - 0.5f + w1 / g0) / numTexels;
    const float2 uv1 = (i + 1.5f + w3 / g1) / numTexels;

    Texture2D<float4> biomeMap = ResourceDescriptorHeap[heapIndices.srv.biomeMapIdx];
    return g0.x * g0.y * biomeMap.SampleLevel(biomeMapSampler, float2(uv0.x, uv0.y), 0.f).rgb
         + g1.x * g0.y * biomeMap.SampleLevel(biomeMapSampler, float2(uv1.x, uv0.y), 0.f).rgb
         + g0.x * g1.y * biomeMap.SampleLevel(biomeMapSampler, float2(uv0.x, uv1.y), 0.f).rgb
         + g1.x * g1.y * biomeMap.SampleLevel(biomeMapSampler, float2(uv1.x, uv1.y), 0.f).rgb;
}

// Returns float4(tint, 1) for biome-tinted triangles and float4(1, 1, 1, 0) otherwise; the
// alpha gates the luminance-replace tint in getMaterialBaseColor.
float4 getBiomeTint(const uint triangleFlags, const float2 posXZ_WS)
{
    if (!bool(triangleFlags & TRIANGLE_FLAG_BIOME_TINT) || sceneParams.biomeMapTexelsPerSide == 0)
    {
        return float4(1.f, 1.f, 1.f, 0.f);
    }

    const float2 blocksXZ_WS = posXZ_WS + float2(cameraParams.globalInstanceOffset.xz);
    const float2 uv = (blocksXZ_WS - float2(sceneParams.biomeMapOriginBlocksXZ_WS))
        / (sceneParams.biomeMapTexelsPerSide * BIOME_MAP_BLOCKS_PER_TEXEL);
    return float4(sampleBiomeMapBicubic(uv), 1.f);
}
