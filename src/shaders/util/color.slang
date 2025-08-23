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

float luminance(const float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 agx_curve3(float3 v) {
    const float threshold = 0.6060606060606061;
    const float a_up = 69.86278913545539;
    const float a_down = 59.507875;
    const float b_up = 13.0 / 4.0;
    const float b_down = 3.0 / 1.0;
    const float c_up = -4.0 / 13.0;
    const float c_down = -1.0 / 3.0;

    float3 mask = step(v, float3(threshold));
    float3 a = a_up + (a_down - a_up) * mask;
    float3 b = b_up + (b_down - b_up) * mask;
    float3 c = c_up + (c_down - c_up) * mask;
    return 0.5 + (((-2.0 * threshold)) + 2.0 * v) * pow(1.0 + a * pow(abs(v - threshold), b), c);
}

float3 agx_tonemapping(float3 ci) {
    const float min_ev = -12.473931188332413;
    const float max_ev = 4.026068811667588;
    const float dynamic_range = max_ev - min_ev;

    const float3x3 agx_mat = float3x3(0.8424010709504686, 0.04240107095046854, 0.04240107095046854, 0.07843650156180276, 0.8784365015618028, 0.07843650156180276, 0.0791624274877287, 0.0791624274877287, 0.8791624274877287);
    const float3x3 agx_mat_inv = float3x3(1.1969986613119143, -0.053001338688085674, -0.053001338688085674, -0.09804562695225345, 1.1519543730477466, -0.09804562695225345, -0.09895303435966087, -0.09895303435966087, 1.151046965640339);

    // Input transform (inset)
    ci = mul(agx_mat, ci);

    // Apply sigmoid function
    float3 ct = saturate(log2(ci) * (1.0 / dynamic_range) - (min_ev / dynamic_range));
    float3 co = agx_curve3(ct);

    // Inverse input transform (outset)
    co = mul(agx_mat_inv, co);

    return co;
}

float3 linearToSRGB(float3 linearColor) {
    bool3 isLower = linearColor < float3(0.0031308);
    float3 higher = float3(1.055) * pow(linearColor, float3(1.0 / 2.4)) - float3(0.055);
    float3 lower = linearColor * float3(12.92);
    return select(isLower, lower, higher);
}

float3 applyTonemapping(float3 color)
{
    return linearToSRGB(color);
    //return linearToSRGB(agx_tonemapping(color));
}
