// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/ggx_tables.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"

// Isotropic GGX microfacet distribution with the height-correlated Smith masking-shadowing term.
// alpha = roughness^2 everywhere.

float ggxDistribution(const float alpha, const float cosThetaH)
{
    const float alpha2 = alpha * alpha;
    const float d = cosThetaH * cosThetaH * (alpha2 - 1.f) + 1.f;
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
float3 sampleGgxVndf(const float3 wo_WS, const float3 surfNor_WS, const float alpha, inout RandomNumberGenerator rng)
{
    const float3x3 tbn = computeTBN(surfNor_WS);
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

// Clamped bilinear lookup of the directional albedo table, matching Cycles' lookup_table_read_2D
float ggxDirectionalAlbedo(const float roughness, const float cosThetaV)
{
    const float x = saturate(roughness) * 31.f;
    const float y = saturate(cosThetaV) * 31.f;
    const uint x0 = min(uint(x), 31u);
    const uint y0 = min(uint(y), 31u);
    const uint x1 = min(x0 + 1u, 31u);
    const uint y1 = min(y0 + 1u, 31u);
    const float row0 = lerp(ggxETable[y0 * 32 + x0], ggxETable[y0 * 32 + x1], x - x0);
    const float row1 = lerp(ggxETable[y1 * 32 + x0], ggxETable[y1 * 32 + x1], x - x0);
    return lerp(row0, row1, y - y0);
}

float ggxAverageAlbedo(const float roughness)
{
    const float x = saturate(roughness) * 31.f;
    const uint x0 = min(uint(x), 31u);
    const uint x1 = min(x0 + 1u, 31u);
    return lerp(ggxEavgTable[x0], ggxEavgTable[x1], x - x0);
}

// Multiple-scattering energy compensation factor for the single-scattering GGX reflection lobe,
// following Cycles' microfacet_ggx_preserve_energy: the lobe is scaled so a white lobe reflects all
// energy, with the multiple-scattering bounces tinted by the single-scattering albedo Fss.
// (Cycles splits this into energy_scale = 1/E and a darkening weight; their product is this factor.)
float3 ggxEnergyCompensation(const float roughness, const float cosThetaV, const float3 fss)
{
    const float e = ggxDirectionalAlbedo(roughness, cosThetaV);
    const float eAvg = ggxAverageAlbedo(roughness);
    const float missingFactor = (1.f - e) / e;
    const float3 fms = fss * eAvg / (1.f - fss * (1.f - eAvg));
    return 1.f + fms * missingFactor;
}

// pdf (in solid angle of wi) of reflecting wo about a VNDF-sampled half vector
float ggxVndfReflectionPdf(const float alpha, const float3 wo_WS, const float3 wi_WS, const float3 surfNor_WS)
{
    const float cosThetaWo = cosTheta(wo_WS, surfNor_WS);
    if (cosThetaWo <= 0.f)
    {
        return 0.f;
    }
    const float3 h_WS = normalize(wo_WS + wi_WS);
    const float d = ggxDistribution(alpha, cosTheta(h_WS, surfNor_WS));
    return ggxSmithG1(alpha, cosThetaWo) * d / (4.f * cosThetaWo);
}
