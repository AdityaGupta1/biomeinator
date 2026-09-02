// SPDX-License-Identifier: Apache-2.0
// Copyright 2011-2022 Blender Foundation
// Ported from Blender Cycles (intern/cycles/kernel/closure/bsdf_util.h, ensure_valid_specular_reflection).
// See external/_licenses/LICENSE_cycles.txt.

#pragma once

// Bends the interpolated shading normal N towards the geometric normal Ng just enough that the reflection of
// wo about N stays above the geometric surface. wo must be on Ng's front side. Interpolated normals near
// silhouettes and on coarse meshes otherwise send reflections and refractions into the surface, and those
// samples have to be discarded, losing energy.
float3 ensureValidSpecularReflection(const float3 Ng, const float3 wo, const float3 N)
{
    const float3 R = 2.f * dot(N, wo) * N - wo;
    const float woZ = dot(wo, Ng);

    // Reflection rays may always be at least as shallow as the incoming ray
    const float threshold = min(0.9f * woZ, 0.01f);
    if (dot(Ng, R) >= threshold)
    {
        return N;
    }

    // Coordinate system with Ng as the Z axis and N in the X-Z plane
    const float3 nTangential = N - dot(N, Ng) * Ng;
    const float nTangentialLenSq = dot(nTangential, nTangential);
    const float3 X = (nTangentialLenSq > 0.f) ? nTangential * rsqrt(nTangentialLenSq) : N;

    // Solve 4*a*Nz^4 - 2*b*Nz^2 + c = 0 for the rotated normal's Nz such that dot(R', Ng) = threshold;
    // see the Cycles source for the derivation
    const float woX = dot(wo, X);
    const float a = woX * woX + woZ * woZ;
    const float b = 2.f * (a + woZ * threshold);
    const float c = (threshold + woZ) * (threshold + woZ);
    const float discriminant = sqrt(max(b * b - 4.f * a * c, 0.f));
    const float Nz2 = (woX < 0.f) ? 0.25f * (b + discriminant) / a : 0.25f * (b - discriminant) / a;

    const float Nx = sqrt(max(1.f - Nz2, 0.f));
    const float Nz = sqrt(max(Nz2, 0.f));
    return Nx * X + Nz * Ng;
}
