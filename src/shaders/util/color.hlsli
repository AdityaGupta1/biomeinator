// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_enums.h"

#include "tonemapping/agx.hlsli"
#include "tonemapping/khronos_pbr_neutral.hlsli"

#include "global_params.hlsli"

float luminance(const float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 srgbToLinear(float3 srgbColor) {
    const float3 higher = pow((srgbColor + 0.055f) / 1.055f, 2.4f);
    const float3 lower = srgbColor / 12.92f;
    return select(srgbColor < 0.04045f, lower, higher);
}

float3 linearToSrgb(float3 linearColor) {
    const float3 higher = 1.055f * pow(linearColor, 1.f / 2.4f) - 0.055f;
    const float3 lower = linearColor * 12.92f;
    return select(linearColor < 0.0031308f, lower, higher);
}

float3 applyReinhard(const float3 color)
{
    const float lum = luminance(color);
    if (lum <= 0.f)
    {
        return color;
    }
    return color / (1.f + lum);
}

float3 applyTonemapping(float3 color)
{
    float3 tonemappedColor;
    switch ((Tonemapping)renderParams.tonemapping)
    {
        case Tonemapping::NONE:
        default:
            tonemappedColor = color;
            break;
        case Tonemapping::STANDARD:
            tonemappedColor = linearToSrgb(color);
            break;
        case Tonemapping::AGX:
            tonemappedColor = linearToSrgb(applyAgx(color));
            break;
        case Tonemapping::KHRONOS_PBR_NEUTRAL:
            tonemappedColor = linearToSrgb(applyKhronosPbrNeutral(color));
            break;
    }
    return tonemappedColor;
}
