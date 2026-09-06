// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/math.hlsli"
#include "util/rng.hlsli"

float3 sampleHemisphereCosineWeighted(const float3 surfNor_WS, inout RandomNumberGenerator rng)
{
    const float2 rndSample = rng.nextFloat2();
    const float r = sqrt(rndSample.x);
    const float theta = M_TWO_PI * rndSample.y;
    const float3 sampledDir_OS = float3(r * cos(theta), r * sin(theta), sqrt(1 - rndSample.x));
    return normalize(mul(computeTBN(surfNor_WS), sampledDir_OS));
}

float hemisphereCosineWeightedPdf(const float3 wi_WS, const float3 surfNor_WS)
{
    return max(cosTheta(wi_WS, surfNor_WS), 0.f) * M_INV_PI;
}

float3 sampleSphericalCapUniform(const float3 axis_WS, const float minCosTheta, inout RandomNumberGenerator rng)
{
    const float2 rndSample = rng.nextFloat2();

    const float cosTheta = lerp(minCosTheta, 1.f, rndSample.x);
    const float sinTheta = sqrt(max(0.f, 1.f - cosTheta * cosTheta));
    const float phi = rndSample.y * M_TWO_PI;

    const float3 sampledDir_OS = float3(
        cos(phi) * sinTheta,
        sin(phi) * sinTheta,
        cosTheta
    );

    return normalize(mul(computeTBN(axis_WS), sampledDir_OS));
}

// pdf of a direction known to lie inside the spherical cap
float sphericalCapUniformPdfInside(const float minCosTheta)
{
    const float omega = M_TWO_PI * (1.f - minCosTheta); // solid angle of entire cone
    return 1.f / omega;
}

// returns 0 if dir_WS is outside the spherical cap
float sphericalCapUniformPdf(const float3 wi_WS, const float3 axis_WS, const float minCosTheta)
{
    if (dot(wi_WS, axis_WS) < minCosTheta)
    {
        return 0.f;
    }
    return sphericalCapUniformPdfInside(minCosTheta);
}
