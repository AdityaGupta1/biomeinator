// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// Layered wave model: two large-scale swells present everywhere, plus three small chop
// waves scaled by a very-low-frequency "choppiness" envelope so some areas read as rough
// water and others as nearly calm. Coefficients hardcoded for now.
// INVARIANT: total amplitude must stay < 0.125 — non-top water verts sit at integer Y,
// which is exactly 0.125 from the nearest k + 7/8 rest position, so the in-place
// vertex identification in water_displace.cs.hlsl breaks if displacement can reach 0.125.
// Current max: swell (0.03 + 0.025) + chop (0.02 + 0.015 + 0.01) = 0.1.

// accumulates one sine wave's height into x and its analytic XZ gradient into yz
void addWave(float amplitude, float2 waveVec, float speed, float2 posXZ_WS, float time, inout float3 heightAndGrad)
{
    const float phase = dot(posXZ_WS, waveVec) + speed * time;
    heightAndGrad.x += amplitude * sin(phase);
    heightAndGrad.yz += amplitude * cos(phase) * waveVec;
}

// returns height in x, d(height)/dx in y, d(height)/dz in z
float3 waveHeightAndGradient(float2 posXZ_WS, float time)
{
    // large-scale swells (~30 block wavelength)
    float3 result = float3(0.f, 0.f, 0.f);
    addWave(0.03f, float2(0.16f, 0.12f), 0.25f, posXZ_WS, time, result);
    addWave(0.025f, float2(-0.11f, 0.19f), 0.3f, posXZ_WS, time, result);

    // choppiness envelope in [0, 1]: product of two slowly drifting low-frequency sines
    // (~300 block patches), so chop fades in and out across the surface
    const float phaseA = dot(posXZ_WS, float2(0.0167f, 0.01f)) + 0.1f * time;
    const float phaseB = dot(posXZ_WS, float2(-0.0067f, 0.02f)) + 0.13f * time;
    const float envelope = 0.5f + 0.5f * sin(phaseA) * sin(phaseB);
    const float2 envelopeGrad = 0.5f * (cos(phaseA) * sin(phaseB) * float2(0.0167f, 0.01f)
                                      + sin(phaseA) * cos(phaseB) * float2(-0.0067f, 0.02f));

    // small-scale chop (~5 block wavelength)
    float3 chop = float3(0.f, 0.f, 0.f);
    addWave(0.02f, float2(0.8f, 0.6f), 0.55f, posXZ_WS, time, chop);
    addWave(0.015f, float2(-0.5f, 1.3f), 0.85f, posXZ_WS, time, chop);
    addWave(0.01f, float2(1.2f, -0.4f), 0.7f, posXZ_WS, time, chop);

    // product rule: d(envelope * chop.x) = envelopeGrad * chop.x + envelope * chop.yz
    result.x += envelope * chop.x;
    result.yz += envelopeGrad * chop.x + envelope * chop.yz;

    return result;
}

float waveHeight(float2 posXZ_WS, float time)
{
    return waveHeightAndGradient(posXZ_WS, time).x;
}

// surface normal = normalize(float3(-grad.x, 1.f, -grad.y))
float2 waveGradient(float2 posXZ_WS, float time)
{
    return waveHeightAndGradient(posXZ_WS, time).yz;
}
