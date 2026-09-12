// SPDX-License-Identifier: Apache-2.0
// Copyright 2011-2022 Blender Foundation
// Ported from Blender Cycles (intern/cycles/kernel/closure/bsdf_util.h, ensure_valid_specular_reflection).
// See external/_licenses/LICENSE_cycles.txt.

#pragma once

// Bends the interpolated shading normal surfShadingNor_WS towards the geometric normal geoNor_WS just enough that the reflection of
// wo about surfShadingNor_WS stays above the geometric surface. wo must be on geoNor_WS's front side. Interpolated normals near
// silhouettes and on coarse meshes otherwise send reflections and refractions into the surface, and those
// samples have to be discarded, losing energy.
float3 ensureValidSpecularReflection(const float3 geoNor_WS, const float3 wo, const float3 surfShadingNor_WS)
{
    const float3 R = 2.f * dot(surfShadingNor_WS, wo) * surfShadingNor_WS - wo;
    const float woZ = dot(wo, geoNor_WS);

    // Reflection rays may always be at least as shallow as the incoming ray
    const float threshold = min(0.9f * woZ, 0.01f);
    if (dot(geoNor_WS, R) >= threshold)
    {
        return surfShadingNor_WS;
    }

    // Coordinate system with geoNor_WS as the Z axis and surfShadingNor_WS in the X-Z plane
    const float3 nTangential = surfShadingNor_WS - dot(surfShadingNor_WS, geoNor_WS) * geoNor_WS;
    const float nTangentialLenSq = dot(nTangential, nTangential);
    const float3 X = (nTangentialLenSq > 0.f) ? nTangential * rsqrt(nTangentialLenSq) : surfShadingNor_WS;

    // Solve 4*a*Nz^4 - 2*b*Nz^2 + c = 0 for the rotated normal's Nz such that dot(R', geoNor_WS) = threshold;
    // see the Cycles source for the derivation
    const float woX = dot(wo, X);
    const float a = woX * woX + woZ * woZ;
    const float b = 2.f * (a + woZ * threshold);
    const float c = (threshold + woZ) * (threshold + woZ);
    const float discriminant = sqrt(max(b * b - 4.f * a * c, 0.f));
    const float Nz2 = (woX < 0.f) ? 0.25f * (b + discriminant) / a : 0.25f * (b - discriminant) / a;

    const float Nx = sqrt(max(1.f - Nz2, 0.f));
    const float Nz = sqrt(max(Nz2, 0.f));
    return Nx * X + Nz * geoNor_WS;
}
