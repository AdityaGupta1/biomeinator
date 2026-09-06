// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#include "util/math.hlsli"
#include "util/packing.hlsli"
#include "util/rng.hlsli"

// How the last vertex of a path was sampled. Paths of different techniques (and lengths) cover
// disjoint parts of path space, so as resampling candidates their MIS weights are all 1.
#define PATH_TECHNIQUE_NEE_AREA 0
#define PATH_TECHNIQUE_NEE_DOME 1
#define PATH_TECHNIQUE_BSDF_EMISSION 2
#define PATH_TECHNIQUE_BSDF_DOME 3

// PathReservoir::flags layout. Vertex indices follow the papers: x1 is the primary hit, the light
// vertex is x_k for a length-k path, and the reconnection vertex x_j has 2 <= j <= k (0 = none).
// The high 16 bits hold the confidence M as 8.8 fixed point.
#define PATH_FLAGS_LENGTH_SHIFT 0
#define PATH_FLAGS_RC_VERTEX_SHIFT 5
#define PATH_FLAGS_TECHNIQUE_SHIFT 10
#define PATH_FLAGS_INDEX_MASK 0x1F
#define PATH_FLAGS_TECHNIQUE_MASK 0x3
#define PATH_FLAGS_SPLIT_IDX (1 << 12)
#define PATH_FLAGS_RC_PREV_LOBE_DIFFUSE (1 << 13) // lobe sampled at the vertex before the rc vertex; else glossy or dielectric
#define PATH_FLAGS_CONFIDENCE_SHIFT 16
#define PATH_FLAGS_CONFIDENCE_MASK 0xFFFF
#define PATH_FLAGS_CONFIDENCE_SCALE 256.f

#define PATH_RC_INSTANCE_GENERATION_SHIFT 24
#define PATH_RC_INSTANCE_ID_MASK 0x00FFFFFF

uint getPathLength(const uint flags)
{
    return (flags >> PATH_FLAGS_LENGTH_SHIFT) & PATH_FLAGS_INDEX_MASK;
}

uint getRcVertexIdx(const uint flags)
{
    return (flags >> PATH_FLAGS_RC_VERTEX_SHIFT) & PATH_FLAGS_INDEX_MASK;
}

uint getPathTechnique(const uint flags)
{
    return (flags >> PATH_FLAGS_TECHNIQUE_SHIFT) & PATH_FLAGS_TECHNIQUE_MASK;
}

bool isDomeTechnique(const uint pathTechnique)
{
    return pathTechnique == PATH_TECHNIQUE_NEE_DOME || pathTechnique == PATH_TECHNIQUE_BSDF_DOME;
}

float reservoirM(const PathReservoir reservoir)
{
    return float((reservoir.flags >> PATH_FLAGS_CONFIDENCE_SHIFT) & PATH_FLAGS_CONFIDENCE_MASK) / PATH_FLAGS_CONFIDENCE_SCALE;
}

void setReservoirM(inout PathReservoir reservoir, const float M)
{
    const uint fixedPoint = min(uint(M * PATH_FLAGS_CONFIDENCE_SCALE + 0.5f), PATH_FLAGS_CONFIDENCE_MASK);
    reservoir.flags = (reservoir.flags & PATH_FLAGS_CONFIDENCE_MASK) | (fixedPoint << PATH_FLAGS_CONFIDENCE_SHIFT);
}

uint rcInstanceId(const PathReservoir reservoir)
{
    return reservoir.rcInstance & PATH_RC_INSTANCE_ID_MASK;
}

uint rcInstanceGeneration(const PathReservoir reservoir)
{
    return reservoir.rcInstance >> PATH_RC_INSTANCE_GENERATION_SHIFT;
}

uint packBarycentrics(const float2 bary2)
{
    const uint2 fixedPoint = uint2(saturate(bary2) * 65535.f + 0.5f);
    return fixedPoint.x | (fixedPoint.y << 16);
}

// Rounding can push the unpacked point just past the triangle's edge, where a reconnection ray to it
// gets occluded by an adjoining surface, so the point is kept on the triangle
float2 unpackBarycentrics(const uint packed)
{
    const float2 bary2 = float2(packed & 0xFFFF, packed >> 16) / 65535.f;
    const float sum = bary2.x + bary2.y;
    return sum > 1.f ? bary2 / sum : bary2;
}

// A complete path produced by the path tracer, as fed to initial resampling
struct PathCandidate
{
    float3 F; // contribution as the path tracer estimates it, including Russian roulette division
    float rrProduct; // product of Russian roulette survival probabilities along the path
    uint pathLength;
    uint pathTechnique;
    uint rcVertexIdx;
    bool rcPrevLobeDiffuse;
    HitInfo rcHit;
    uint rcInstanceGeneration;
    float3 rcWi;
    float3 rcRadiance;
    float rcLightPdf;
    float rcJacobianTerms;
};

PathReservoir makeEmptyPathReservoir()
{
    PathReservoir reservoir;
    reservoir.F = 0.f;
    reservoir.W = 0.f;
    reservoir.seed = 0;
    reservoir.flags = 0;
    reservoir.rcLightPdf = 0.f;
    reservoir.rcJacobianTerms = 0.f;
    reservoir.rcInstance = 0;
    reservoir.rcTriangleIdx = 0;
    reservoir.rcBarycentrics = 0;
    reservoir.rcWi = 0;
    reservoir.rcRadiance = 0.f;
    reservoir.debugFlags = 0;
    return reservoir;
}

// Weighted reservoir sampling over the complete paths of one path tree (initial resampling in
// ReSTIR PT). Each candidate's F is its full contribution as the path tracer already estimates it,
// so in primary sample space the source pdf is the Russian roulette survival product and the
// resampling weight luminance(F_noRR) / rrProduct is just luminance(F). The stored integrand
// excludes the roulette division, since random replay never applies roulette.
struct PathTreeReservoir
{
    PathReservoir selected;
    float weightSum;
    RandomNumberGenerator rng;
    uint seed;
    uint splitIdx;

    void addCandidate(const PathCandidate candidate)
    {
        const float weight = luminance(candidate.F);
        if (weight <= 0.f)
        {
            return;
        }

        weightSum += weight;
        if (rng.nextFloat() * weightSum >= weight)
        {
            return;
        }

        selected.F = candidate.F * candidate.rrProduct;
        selected.seed = seed;
        selected.flags = (candidate.pathLength << PATH_FLAGS_LENGTH_SHIFT) |
                         (candidate.rcVertexIdx << PATH_FLAGS_RC_VERTEX_SHIFT) |
                         (candidate.pathTechnique << PATH_FLAGS_TECHNIQUE_SHIFT) |
                         (splitIdx != 0 ? PATH_FLAGS_SPLIT_IDX : 0) |
                         (candidate.rcPrevLobeDiffuse ? PATH_FLAGS_RC_PREV_LOBE_DIFFUSE : 0);
        selected.rcLightPdf = candidate.rcLightPdf;
        selected.rcJacobianTerms = candidate.rcJacobianTerms;
        selected.rcInstance = (candidate.rcInstanceGeneration << PATH_RC_INSTANCE_GENERATION_SHIFT) |
                              (candidate.rcHit.instanceId & PATH_RC_INSTANCE_ID_MASK);
        selected.rcTriangleIdx = candidate.rcHit.triangleIdx;
        selected.rcBarycentrics = packBarycentrics(candidate.rcHit.barycentrics);
        selected.rcWi = octEncode(candidate.rcWi);
        selected.rcRadiance = candidate.rcRadiance;
    }

    // W = weightSum / pHat(selected). One path tree is one unit of confidence, empty or not.
    PathReservoir finalize()
    {
        PathReservoir result = selected;
        const float pHat = luminance(result.F);
        result.W = (weightSum > 0.f && pHat > 0.f) ? weightSum / pHat : 0.f;
        setReservoirM(result, 1.f);
        return result;
    }
};

PathTreeReservoir initPathTreeReservoir(const RandomNumberGenerator rng, const uint seed, const uint splitIdx)
{
    PathTreeReservoir reservoir;
    reservoir.selected = makeEmptyPathReservoir();
    reservoir.weightSum = 0.f;
    reservoir.rng = rng;
    reservoir.seed = seed;
    reservoir.splitIdx = splitIdx;
    return reservoir;
}
