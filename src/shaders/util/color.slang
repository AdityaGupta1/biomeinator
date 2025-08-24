/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "tonemapping/agx.slang"
#include "tonemapping/khronos_pbr_neutral.slang"

float luminance(const float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 srgbToLinear(float3 srgbColor) {
    const bool3 isLower = srgbColor < float3(0.04045);
    const float3 higher = pow((srgbColor + float3(0.055f)) / float3(1.055f), float3(2.4f));
    const float3 lower = srgbColor / float3(12.92);

    return select(isLower, lower, higher);
}

float3 linearToSrgb(float3 linearColor) {
    const bool3 isLower = linearColor < float3(0.0031308f);
    const float3 higher = float3(1.055f) * pow(linearColor, float3(1.f / 2.4f)) - float3(0.055f);
    const float3 lower = linearColor * float3(12.92f);
    return select(isLower, lower, higher);
}

float3 applyTonemapping(float3 color)
{
    return linearToSrgb(applyKhronosPbrNeutral(color));
    // return linearToSrgb(applyAgx(color));
}
