// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#include "util/math.hlsli"
#include "util/rng.hlsli"

// How the last vertex of a path was sampled. Paths of different techniques (and lengths) cover
// disjoint parts of path space, so as resampling candidates their MIS weights are all 1.
#define PATH_TECHNIQUE_NEE_AREA 0
#define PATH_TECHNIQUE_NEE_DOME 1
#define PATH_TECHNIQUE_BSDF_EMISSION 2
#define PATH_TECHNIQUE_BSDF_DOME 3

// PathReservoir::flags layout. Vertex indices follow the papers: x1 is the primary hit, the light
// vertex is x_k for a length-k path, and the reconnection vertex x_j has 2 <= j <= k (0 = none).
#define PATH_FLAGS_LENGTH_SHIFT 0
#define PATH_FLAGS_RC_VERTEX_SHIFT 5
#define PATH_FLAGS_TECHNIQUE_SHIFT 10
#define PATH_FLAGS_INDEX_MASK 0x1F
#define PATH_FLAGS_TECHNIQUE_MASK 0x3
#define PATH_FLAGS_SPLIT_IDX (1 << 12)
#define PATH_FLAGS_RC_HIT_BACKFACE (1 << 13)
#define PATH_FLAGS_RC_PREV_LOBE_DIFFUSE (1 << 14) // lobe sampled at the vertex before the rc vertex; else glossy or dielectric

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

// A complete path produced by the path tracer, as fed to initial resampling
struct PathCandidate
{
    float3 F; // contribution as the path tracer estimates it, including Russian roulette division
    float rrProduct; // product of Russian roulette survival probabilities along the path
    uint pathLength;
    uint pathTechnique;
    uint rcVertexIdx;
    bool rcHitIsBackface;
    bool rcPrevLobeDiffuse;
    HitInfo rcHit;
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
    reservoir.M = 0.f;
    reservoir.rcLightPdf = 0.f;
    reservoir.rcHit.hitPos_WS = 0.f;
    reservoir.rcHit.instanceId = 0;
    reservoir.rcHit.hitNor_WS = 0.f;
    reservoir.rcHit.triangleIdx = 0;
    reservoir.rcHit.uv = 0.f;
    reservoir.rcHit.pad0 = 0;
    reservoir.rcHit.pad1 = 0;
    reservoir.rcWi = 0.f;
    reservoir.rcJacobianTerms = 0.f;
    reservoir.rcRadiance = 0.f;
    reservoir.pad0 = 0;
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
                         (candidate.rcHitIsBackface ? PATH_FLAGS_RC_HIT_BACKFACE : 0) |
                         (candidate.rcPrevLobeDiffuse ? PATH_FLAGS_RC_PREV_LOBE_DIFFUSE : 0);
        selected.rcLightPdf = candidate.rcLightPdf;
        selected.rcHit = candidate.rcHit;
        selected.rcWi = candidate.rcWi;
        selected.rcJacobianTerms = candidate.rcJacobianTerms;
        selected.rcRadiance = candidate.rcRadiance;
    }

    // W = weightSum / pHat(selected). One path tree is one unit of confidence, empty or not.
    PathReservoir finalize()
    {
        PathReservoir result = selected;
        const float pHat = luminance(result.F);
        result.W = (weightSum > 0.f && pHat > 0.f) ? weightSum / pHat : 0.f;
        result.M = 1.f;
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
