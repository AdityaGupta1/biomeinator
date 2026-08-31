// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#define M_PI       3.14159265358979323846f
#define M_TWO_PI   6.28318530717958647692f
#define M_INV_PI   0.31830988618379067153f

float3x3 computeTBN(const float3 normal)
{
    const float3 up = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = normalize(cross(normal, tangent));
    return float3x3(
        tangent.x, bitangent.x, normal.x,
        tangent.y, bitangent.y, normal.y,
        tangent.z, bitangent.z, normal.z
    );
}

float3 faceforward(const float3 vec, const float3 ref)
{
    return (dot(vec, ref) < 0.f) ? -vec : vec;
}

float cosTheta(const float3 v_WS, const float3 normal_WS)
{
    return dot(v_WS, normal_WS);
}

float absCosTheta(const float3 v_WS, const float3 normal_WS)
{
    return abs(cosTheta(v_WS, normal_WS));
}

float distance2(const float3 a, const float3 b)
{
    const float3 aToB = a - b;
    return dot(aToB, aToB);
}

float balanceHeuristic(const float pdfA, const float pdfB)
{
    return pdfA / (pdfA + pdfB);
}

float luminance(const float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}
