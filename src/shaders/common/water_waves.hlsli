// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_water_waves.h"

#include "util/FastNoiseLite.hlsli"

// Layered wave model. Vertex displacement (waveHeight) is a pure analytic sum of sines:
// two large-scale swells present everywhere, plus three small chop waves scaled by a
// very-low-frequency "choppiness" envelope so some areas read as rough water and others
// as nearly calm.
// INVARIANT: total displacement amplitude must stay < 0.125: non-top water verts sit at
// integer Y, which is exactly 0.125 from the nearest k + 7/8 rest position, so the in-place
// vertex identification in water_displace.cs.hlsl breaks if displacement can reach 0.125.
// Current max: sum(SWELL_STRENGTHS) 0.055 + sum(CHOP_STRENGTHS) 0.045 = 0.1.
//
// The shading normal (waveShadingNormal) adds a noise-based perturbation
// (waveNormalPerturbation) on top of the analytic sine gradient: small-scale OpenSimplex2
// FBM tilts the normal in X and Z. It never touches waveHeight, so it adds surface detail
// without moving verts (and cannot break the invariant). The perturbation is scaled by a
// choppiness factor combining the large-scale sine envelope with a medium-scale
// OpenSimplex2 envelope, floored so even calm water keeps some detail.
//
// Wave parameters come in triples: STRENGTH (amplitude), FREQ (angular wave vector; its
// magnitude sets the spatial frequency) and SPEED (time scale).

// ===== Displacement =====
// Sine waves that move the vertices (waveHeight) and, via their analytic gradient, also
// define the base shading normal.

// large-scale swells, present everywhere (~30 block wavelength)
static const int SWELL_WAVE_COUNT = WATER_SWELL_WAVE_COUNT;
static const float SWELL_STRENGTHS[SWELL_WAVE_COUNT] = WATER_SWELL_STRENGTHS;
static const float2 SWELL_FREQS[SWELL_WAVE_COUNT] = WATER_SWELL_FREQS;
static const float SWELL_SPEEDS[SWELL_WAVE_COUNT] = WATER_SWELL_SPEEDS;

// small-scale chop, gated by the shared choppiness envelope below (~5 block wavelength)
static const int CHOP_WAVE_COUNT = WATER_CHOP_WAVE_COUNT;
static const float CHOP_STRENGTHS[CHOP_WAVE_COUNT] = WATER_CHOP_STRENGTHS;
static const float2 CHOP_FREQS[CHOP_WAVE_COUNT] = WATER_CHOP_FREQS;
static const float CHOP_SPEEDS[CHOP_WAVE_COUNT] = WATER_CHOP_SPEEDS;

// ===== Shared: choppiness envelope =====
// Large-scale sine/cos factor in [0, 1] (~300 block patches). Gates the sine chop in the
// displacement above AND scales the noise perturbation below, so calm and rough regions
// line up between geometry and shading detail.
static const float2 SINE_CHOP_FREQS[2] = WATER_SINE_CHOP_FREQS;
static const float2 SINE_CHOP_SPEEDS = WATER_SINE_CHOP_SPEEDS;

// ===== Normal perturbation =====
// Noise applied to the shading normal only (waveNormalPerturbation); never moves vertices.

// medium-scale OpenSimplex2 choppiness, multiplied into the shared factor above
static const float MED_CHOP_FREQ = 0.02f;
static const float MED_CHOP_SPEED = 0.05f;

// small-scale OpenSimplex2 FBM tilt
static const float NOISE_WAVE_FREQ = 0.5f;
static const float NOISE_WAVE_SPEED = 0.2f;
static const float NOISE_PERTURB_STRENGTH = 0.12f;
static const int NOISE_WAVE_OCTAVES = 4;

static const float CHOP_FLOOR = 0.1f; // choppiness retained in the calmest areas

// frequency stays 1 because the spatial scale is applied to the sample inputs directly
fnl_state makeNoiseState(int seed, int fractalType, int octaves)
{
    fnl_state state = fnlCreateState(seed);
    state.noise_type = FNL_NOISE_OPENSIMPLEX2;
    state.fractal_type = fractalType;
    state.octaves = octaves;
    state.frequency = 1.f;
    return state;
}

static const fnl_state MED_CHOP_NOISE_STATE = makeNoiseState(9001, FNL_FRACTAL_NONE, 1);
static const fnl_state NOISE_WAVE_STATE = makeNoiseState(1337, FNL_FRACTAL_FBM, NOISE_WAVE_OCTAVES);

// accumulates one sine wave's height into x and its analytic XZ gradient into yz
void addWave(float amplitude, float2 waveVec, float speed, float2 posXZ_WS, float time, inout float3 heightAndGrad)
{
    const float phase = dot(posXZ_WS, waveVec) + speed * time;
    heightAndGrad.x += amplitude * sin(phase);
    heightAndGrad.yz += amplitude * cos(phase) * waveVec;
}

// large-scale sine/cos choppiness envelope: value in [0, 1] in x, XZ gradient in yz
float3 sineChop01AndGradient(float2 posXZ_WS, float time)
{
    const float phaseA = dot(posXZ_WS, SINE_CHOP_FREQS[0]) + SINE_CHOP_SPEEDS.x * time;
    const float phaseB = dot(posXZ_WS, SINE_CHOP_FREQS[1]) + SINE_CHOP_SPEEDS.y * time;
    const float2 grad = 0.5f * (cos(phaseA) * sin(phaseB) * SINE_CHOP_FREQS[0]
                              + sin(phaseA) * cos(phaseB) * SINE_CHOP_FREQS[1]);
    return float3(0.5f + 0.5f * sin(phaseA) * sin(phaseB), grad);
}

float sineChop01(float2 posXZ_WS, float time)
{
    return sineChop01AndGradient(posXZ_WS, time).x;
}

// returns height in x, d(height)/dx in y, d(height)/dz in z
float3 waveHeightAndGradient(float2 posXZ_WS, float time)
{
    float3 result = float3(0.f, 0.f, 0.f);
    [unroll]
    for (int i = 0; i < SWELL_WAVE_COUNT; i++)
    {
        addWave(SWELL_STRENGTHS[i], SWELL_FREQS[i], SWELL_SPEEDS[i], posXZ_WS, time, result);
    }

    const float3 envelope = sineChop01AndGradient(posXZ_WS, time);

    float3 chop = float3(0.f, 0.f, 0.f);
    [unroll]
    for (int j = 0; j < CHOP_WAVE_COUNT; j++)
    {
        addWave(CHOP_STRENGTHS[j], CHOP_FREQS[j], CHOP_SPEEDS[j], posXZ_WS, time, chop);
    }

    // product rule: d(envelope * chop.x) = envelopeGrad * chop.x + envelope * chopGrad
    result.x += envelope.x * chop.x;
    result.yz += envelope.yz * chop.x + envelope.x * chop.yz;

    return result;
}

float waveHeight(float2 posXZ_WS, float time)
{
    return waveHeightAndGradient(posXZ_WS, time).x;
}

// medium-scale OpenSimplex2 choppiness envelope in [0, 1]
float medChop01(float2 posXZ_WS, float time)
{
    const float n = fnlGetNoise3D(MED_CHOP_NOISE_STATE, posXZ_WS.x * MED_CHOP_FREQ, posXZ_WS.y * MED_CHOP_FREQ, time * MED_CHOP_SPEED);
    return 0.5f + 0.5f * n;
}

// noise tilt added to the shading normal via the wave gradient. Two decorrelated
// OpenSimplex2 FBM samples give independent X and Z perturbation, scaled by a choppiness
// factor (large-scale sine envelope * medium-scale noise envelope), floored at CHOP_FLOOR.
float2 waveNormalPerturbation(float2 posXZ_WS, float time)
{
    const float chop = CHOP_FLOOR + (1.f - CHOP_FLOOR) * sineChop01(posXZ_WS, time) * medChop01(posXZ_WS, time);

    const float3 samplePos = float3(posXZ_WS * NOISE_WAVE_FREQ, time * NOISE_WAVE_SPEED);
    const float3 decorrelated = samplePos + float3(137.f, -91.f, 0.f) * NOISE_WAVE_FREQ;
    const float nx = fnlGetNoise3D(NOISE_WAVE_STATE, samplePos.x, samplePos.y, samplePos.z);
    const float nz = fnlGetNoise3D(NOISE_WAVE_STATE, decorrelated.x, decorrelated.y, decorrelated.z);

    return chop * NOISE_PERTURB_STRENGTH * float2(nx, nz);
}

// Shading normal for a water top-surface hit: the analytic wave gradient plus the noise
// perturbation, flipped for backface (underwater) hits.
//
// At grazing incidence, the perturbed normal can reflect rays into this or a neighboring
// wave, where they return almost no light (mostly lost to volume absorption), showing as
// flickering black pixels. Clamp the reflection direction to a margin above the unperturbed
// surface's horizon (enough to clear neighboring waves) and rebuild the normal as the
// view/reflection half vector, which also keeps the normal in the viewer's hemisphere.
float3 waveShadingNormal(float2 posXZ_WS, float time, float3 rayDir_WS, bool backfaceHit)
{
    const float2 baseGrad = waveHeightAndGradient(posXZ_WS, time).yz;
    const float2 grad = baseGrad + waveNormalPerturbation(posXZ_WS, time);
    const float flip = backfaceHit ? -1.f : 1.f;
    const float3 baseNor_WS = flip * normalize(float3(-baseGrad.x, 1.f, -baseGrad.y));
    float3 waveNor_WS = flip * normalize(float3(-grad.x, 1.f, -grad.y));

    const float minReflectedDotBase = 0.05f;
    const float3 reflected_WS = reflect(rayDir_WS, waveNor_WS);
    const float reflectedDotBase = dot(reflected_WS, baseNor_WS);
    if (reflectedDotBase < minReflectedDotBase)
    {
        const float3 clampedReflected_WS = normalize(reflected_WS + baseNor_WS * (minReflectedDotBase - reflectedDotBase));
        const float3 halfVec_WS = clampedReflected_WS - rayDir_WS; // view + clamped reflection
        // the half vector degenerates when the clamped reflection points back along the
        // ray; fall back to the unperturbed normal
        waveNor_WS = dot(halfVec_WS, halfVec_WS) > 1e-6f ? normalize(halfVec_WS) : baseNor_WS;
    }
    return waveNor_WS;
}
