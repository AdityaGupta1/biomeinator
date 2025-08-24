// MIT License
//
// Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// All values used to derive this implementation are sourced from Troy's initial AgX implementation/OCIO config file available here:
//   https://github.com/sobotka/AgX

#pragma once

float3 srgbToLinear(float3 srgbColor) {
    bool3 isLower = srgbColor < float3(0.04045);
    float3 higher = pow((srgbColor + float3(0.055f)) / float3(1.055f), float3(2.4f));
    float3 lower = srgbColor / float3(12.92);

    return select(isLower, lower, higher);
}

float3 linearToSrgb(float3 linearColor) {
    bool3 isLower = linearColor < float3(0.0031308f);
    float3 higher = float3(1.055f) * pow(linearColor, float3(1.f / 2.4f)) - float3(0.055f);
    float3 lower = linearColor * float3(12.92f);
    return select(isLower, lower, higher);
}

// Mean error^2: 1.85907662e-06
float3 agxDefaultContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    float3 x6 = x4 * x2;

    return -17.86f * x6 * x
         + 78.01f * x6
         - 126.7f * x4 * x
         + 92.06f * x4
         - 28.72f * x2 * x
         + 4.361f * x2
         - 0.1718f * x
         + 0.002857f;
}

float3 agx(float3 val) {
    const float3x3 agx_mat = float3x3(
        0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f,
        0.0784335999999992f, 0.878468636469772f, 0.0784336f,
        0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f
    );

    const float min_ev = -12.47393f;
    const float max_ev = 4.026069f;

    // Input transform (inset)
    val = mul(val, agx_mat);
    //val = mul(agx_mat, val);

    // Log2 space encoding
    val = clamp(log2(val), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);

    // Apply sigmoid function approximation
    val = agxDefaultContrastApprox(val);

    return val;
}

float3 agxEotf(float3 val) {
    const float3x3 agx_mat_inv = float3x3(
        1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f,
        -0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f,
        -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f
    );

    // Inverse input transform (outset)
    val = mul(val, agx_mat_inv);
    // val = mul(agx_mat_inv, val);

    // sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
    // NOTE: We're linearizing the output here. Comment/adjust when
    // *not* using a sRGB render target
    val = srgbToLinear(val);

    return val;
}

float3 applyAgx(float3 color)
{
    color = agx(color);
    color = agxEotf(color);
    return color;
}
