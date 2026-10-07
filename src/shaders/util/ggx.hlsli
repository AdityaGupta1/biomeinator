// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "util/ggx_fresnel_tables.hlsli"
#include "util/ggx_tables.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"

// Isotropic GGX microfacet distribution with the height-correlated Smith masking-shadowing term.
// The distribution/masking functions take alpha = roughness^2; the albedo table lookups take
// roughness directly, matching the tables' parameterization.

float ggxDistribution(const float alpha, const float cosThetaH)
{
    const float alpha2 = alpha * alpha;
    // Keep alpha^2 out of the subtraction from one: smooth roughness-map texels
    // otherwise round the denominator to zero at the specular peak (Inf/NaN).
    const float cos2 = saturate(cosThetaH * cosThetaH);
    precise float d = (1.f - cos2) + cos2 * alpha2;
    return alpha2 * M_INV_PI / (d * d);
}

float ggxSmithLambda(const float alpha, const float cosThetaV)
{
    const float cos2 = cosThetaV * cosThetaV;
    const float tan2 = max(1.f - cos2, 0.f) / cos2;
    return 0.5f * (sqrt(1.f + alpha * alpha * tan2) - 1.f);
}

float ggxSmithG1(const float alpha, const float cosThetaV)
{
    return 1.f / (1.f + ggxSmithLambda(alpha, cosThetaV));
}

float ggxSmithG2(const float alpha, const float cosThetaWo, const float cosThetaWi)
{
    return 1.f / (1.f + ggxSmithLambda(alpha, cosThetaWo) + ggxSmithLambda(alpha, cosThetaWi));
}

// "Sampling the GGX Distribution of Visible Normals", Heitz, 2018
// Returns a half vector; the caller reflects wo about it to get wi.
float3 sampleGgxVndf(const float3 wo_WS, const float3 surfShadingNor_WS, const float alpha, inout RandomNumberGenerator rng)
{
    const float3x3 tbn = computeTBN(surfShadingNor_WS);
    const float3 wo_TS = mul(wo_WS, tbn);

    const float3 vh = normalize(float3(alpha * wo_TS.x, alpha * wo_TS.y, wo_TS.z));

    const float lenSq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = (lenSq > 0.f) ? float3(-vh.y, vh.x, 0.f) / sqrt(lenSq) : float3(1.f, 0.f, 0.f);
    const float3 t2 = cross(vh, t1);

    const float2 rndSample = rng.nextFloat2();
    const float r = sqrt(rndSample.x);
    const float phi = M_TWO_PI * rndSample.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5f * (1.f + vh.z);
    p2 = (1.f - s) * sqrt(max(1.f - p1 * p1, 0.f)) + s * p2;

    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(1.f - p1 * p1 - p2 * p2, 0.f)) * vh;
    const float3 h_TS = normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.f)));

    return normalize(mul(tbn, h_TS));
}

static const uint GGX_TABLE_SIZE = 32u; // per axis of ggxETable / ggxEavgTable
static const uint GGX_GLASS_TABLE_SIZE = 16u; // per axis of the ggxGlass*Table set

// Maps a [0, 1] table coordinate to the bracketing indices of a size-wide axis and returns the lerp factor between
// them; clamped like Cycles' lookup_table_read
float tableAxis(const float coord, const uint size, out uint idx0, out uint idx1)
{
    const float x = saturate(coord) * float(size - 1u);
    idx0 = min(uint(x), size - 1u);
    idx1 = min(idx0 + 1u, size - 1u);
    return x - idx0;
}

float ggxDirectionalAlbedo(const float roughness, const float cosThetaV)
{
    uint x0, x1, y0, y1;
    const float tx = tableAxis(roughness, GGX_TABLE_SIZE, x0, x1);
    const float ty = tableAxis(cosThetaV, GGX_TABLE_SIZE, y0, y1);
    const float row0 = lerp(ggxETable[y0 * GGX_TABLE_SIZE + x0], ggxETable[y0 * GGX_TABLE_SIZE + x1], tx);
    const float row1 = lerp(ggxETable[y1 * GGX_TABLE_SIZE + x0], ggxETable[y1 * GGX_TABLE_SIZE + x1], tx);
    return lerp(row0, row1, ty);
}

float ggxAverageAlbedo(const float roughness)
{
    uint x0, x1;
    const float tx = tableAxis(roughness, GGX_TABLE_SIZE, x0, x1);
    return lerp(ggxEavgTable[x0], ggxEavgTable[x1], tx);
}

// The ior-indexed tables address their ior axis by z = sqrt((ior - 1) / (ior + 1)) for ior >= 1; a relative
// ior below 1 uses the inverse table with 1/ior
float ggxTableZ(const float ior, out bool useInverseTable)
{
    useInverseTable = ior < 1.f;
    const float tableIor = useInverseTable ? 1.f / ior : ior;
    return sqrt((tableIor - 1.f) / (tableIor + 1.f));
}

// Corner indices and lerp weights of a trilinear lookup in a table indexed [z][cosThetaV][roughness]
struct Table3DTaps
{
    uint idx[8]; // corner (x, y, z) at x + 2y + 4z
    float3 t;
};

// size holds the roughness, cosThetaV and z axis lengths
Table3DTaps ggxTable3DTaps(const uint3 size, const float roughness, const float cosThetaV, const float z)
{
    uint x[2], y[2], zs[2];
    Table3DTaps taps;
    taps.t = float3(tableAxis(roughness, size.x, x[0], x[1]), tableAxis(cosThetaV, size.y, y[0], y[1]),
                    tableAxis(z, size.z, zs[0], zs[1]));
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        taps.idx[i] = (zs[i >> 2] * size.y + y[(i >> 1) & 1]) * size.x + x[i & 1];
    }
    return taps;
}

float ggxTable3DBlend(const float v[8], const float3 t)
{
    const float4 alongX = lerp(float4(v[0], v[2], v[4], v[6]), float4(v[1], v[3], v[5], v[7]), t.x);
    const float2 alongY = lerp(alongX.xz, alongX.yw, t.y);
    return lerp(alongY.x, alongY.y, t.z);
}

float ggxGlassEavgTableRead(const bool useInverseTable, const uint idx)
{
    return useInverseTable ? ggxGlassInvEavgTable[idx] : ggxGlassEavgTable[idx];
}

float ggxGlassDirectionalAlbedo(const float roughness, const float cosThetaV, const float ior)
{
    bool useInverseTable;
    const float z = ggxTableZ(ior, useInverseTable);
    const Table3DTaps taps = ggxTable3DTaps(uint3(GGX_GLASS_TABLE_SIZE, GGX_GLASS_TABLE_SIZE, GGX_GLASS_TABLE_SIZE),
                                            roughness, cosThetaV, z);
    float v[8];
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        v[i] = useInverseTable ? ggxGlassInvETable[taps.idx[i]] : ggxGlassETable[taps.idx[i]];
    }
    return ggxTable3DBlend(v, taps.t);
}

// Angle axis of the inverse Fresnel table: the critical angle's cosine maps to the axis midpoint, so the total
// internal reflection ramp lines up across the table's ior slices instead of being blurred between them
float ggxCriticalCosAxis(const float cosThetaV, const float ior)
{
    const float criticalCos = sqrt(max(1.f - ior * ior, 0.f));
    return (cosThetaV < criticalCos) ? 0.5f * cosThetaV / criticalCos
                                     : 0.5f + 0.5f * (cosThetaV - criticalCos) / (1.f - criticalCos);
}

// Average dielectric Fresnel reflectance of the visible microfacets seen from wo: the share of light a glossy
// reflection lobe with per-microfacet Fresnel reflects
float ggxFresnelAlbedo(const float roughness, const float cosThetaV, const float ior)
{
    bool useInverseTable;
    const float z = ggxTableZ(ior, useInverseTable);
    const Table3DTaps taps = ggxTable3DTaps(GGX_FRESNEL_TABLE_SIZE, roughness,
        useInverseTable ? ggxCriticalCosAxis(cosThetaV, ior) : cosThetaV, z / GGX_FRESNEL_TABLE_MAX_Z);
    const uint base = useInverseTable ? GGX_FRESNEL_INV_TABLE_OFFSET : 0u;
    float v[8];
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        v[i] = ggxFresnelETable[base + taps.idx[i]];
    }
    return ggxTable3DBlend(v, taps.t);
}

float ggxGlassAverageAlbedo(const float roughness, const float ior)
{
    bool useInverseTable;
    const float z = ggxTableZ(ior, useInverseTable);
    uint x0, x1, z0, z1;
    const float tx = tableAxis(roughness, GGX_GLASS_TABLE_SIZE, x0, x1);
    const float tz = tableAxis(z, GGX_GLASS_TABLE_SIZE, z0, z1);
    const uint row0 = z0 * GGX_GLASS_TABLE_SIZE;
    const uint row1 = z1 * GGX_GLASS_TABLE_SIZE;
    const float e0 = lerp(ggxGlassEavgTableRead(useInverseTable, row0 + x0), ggxGlassEavgTableRead(useInverseTable, row0 + x1), tx);
    const float e1 = lerp(ggxGlassEavgTableRead(useInverseTable, row1 + x0), ggxGlassEavgTableRead(useInverseTable, row1 + x1), tx);
    return lerp(e0, e1, tz);
}

// Multiple-scattering energy compensation factor for a single-scattering GGX lobe with directional albedo e and
// average albedo eAvg, following Cycles' microfacet_ggx_preserve_energy: the lobe is scaled so a white lobe
// scatters all energy, with the multiple-scattering bounces tinted by the single-scattering albedo Fss.
// (Cycles splits this into energy_scale = 1/E and a darkening weight; their product is this factor.)
float3 ggxEnergyCompensationFromAlbedo(const float e, const float eAvg, const float3 fss)
{
    const float missingFactor = (1.f - e) / e;
    const float3 fms = fss * eAvg / (1.f - fss * (1.f - eAvg));
    return 1.f + fms * missingFactor;
}

float3 ggxEnergyCompensation(const float roughness, const float cosThetaV, const float3 fss)
{
    return ggxEnergyCompensationFromAlbedo(ggxDirectionalAlbedo(roughness, cosThetaV), ggxAverageAlbedo(roughness), fss);
}

// For the dielectric (reflection + transmission) lobe
float3 ggxGlassEnergyCompensation(const float roughness, const float cosThetaV, const float ior, const float3 fss)
{
    return ggxEnergyCompensationFromAlbedo(
        ggxGlassDirectionalAlbedo(roughness, cosThetaV, ior), ggxGlassAverageAlbedo(roughness, ior), fss);
}

// "Microfacet Models for Refraction through Rough Surfaces", Walter et al., 2007. ior is the relative ior of the
// side wi is on; the half vector (eq. 16) points towards the lower-ior side, and the Jacobian (eq. 17) maps the
// half vector's solid angle to wi's.
float3 refractionHalfVector(const float3 wo_WS, const float3 wi_WS, const float ior)
{
    return normalize(-(ior * wi_WS + wo_WS));
}

float refractionJacobian(const float ior, const float cosThetaWoH, const float cosThetaWiH)
{
    const float denom = ior * cosThetaWiH + cosThetaWoH;
    return ior * ior * abs(cosThetaWiH) / (denom * denom);
}
