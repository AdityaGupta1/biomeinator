#pragma once

#define PI      3.14159265358979323846f
#define TWO_PI  6.28318530717958647692f

float3x3 computeTBN(const float3 normal)
{
    const float3 tangent = normalize(any(normal.x) ? cross(normal, float3(0, 1, 0)) : cross(normal, float3(1, 0, 0)));
    const float3 bitangent = normalize(cross(normal, tangent));
    return float3x3(tangent, bitangent, normal);
}

float3 sampleHemisphereCosineWeighted(const float3 normal_WS, const float2 rndSample)
{
    const float r = sqrt(rndSample.x);
    const float theta = TWO_PI * rndSample.y;
    const float3 sampledDir_OS = float3(r * cos(theta), r * sin(theta), sqrt(1 - rndSample.x));
    return mul(computeTBN(normal_WS), sampledDir_OS);
}
