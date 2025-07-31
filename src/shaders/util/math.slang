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

float3 sampleHemisphereCosineWeighted(const float3 normal_WS, const float2 rndSample)
{
    const float r = sqrt(rndSample.x);
    const float theta = M_TWO_PI * rndSample.y;
    const float3 sampledDir_OS = float3(r * cos(theta), r * sin(theta), sqrt(1 - rndSample.x));
    return mul(computeTBN(normal_WS), sampledDir_OS);
}

float3 faceforward(const float3 normal, const float3 vec)
{
    return (dot(normal, vec) < 0.f) ? -normal : normal;
}

float absCosTheta(const float3 v_WS, const float3 normal_WS)
{
    return abs(dot(v_WS, normal_WS));
}

float distance2(const float3 a, const float3 b)
{
    return dot(a - b, a - b);
}
