// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once
#include "util/sampling.hlsli"

// Deliberately larger than the real sun (~0.8 degree radius).
static const float sunCosTheta = 0.9999f;
static const float sunSolidAngle = M_TWO_PI * (1.f - sunCosTheta);

// One uniform solid-angle sample. Reuse this direction for every part of the
// lighting estimate (phase/BRDF, atmosphere, terrain and cloud visibility).
float3 sampleSunDirection(float3 center, inout RandomNumberGenerator rng)
{
    return sampleSphericalCapUniform(center, sunCosTheta, rng);
}
