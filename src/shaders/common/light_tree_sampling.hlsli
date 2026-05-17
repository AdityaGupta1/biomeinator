// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

// Real-Time Stochastic Lightcuts (Lin & Yuksel 2020) HIS sampler + MIS pdf
// recovery. Reads the perfect-binary tree built by LightTreeManager
// (Stage 1/2 of plans/plan.md). Per-pixel root descent — no cut sharing yet
// (Stage 5 lands that).

#ifndef LIGHT_TREE_SAMPLING_HLSLI
#define LIGHT_TREE_SAMPLING_HLSLI

// Include paths mirror the convention of light/light_sampling.hlsli so that
// DXC's #pragma once sees the same canonical path string from every shader
// translation unit — otherwise we hit redefinition errors on the util/* headers.
#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "common/global_params.hlsli"
#include "common/light_tree.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"

// Bound via PtParam::RTSL_LIGHT_TREE / RTSL_LIGHT_TO_LEAF in renderer_pipeline.cpp.
// Constants live in rtslParams (in GlobalParams cbuffer via global_params.hlsli).
StructuredBuffer<LightTreeNode> rtslLightTree   : REGISTER_T(LIGHT_TREE, LIGHT_TREE_IN);
StructuredBuffer<uint>          rtslLightToLeaf : REGISTER_T(LIGHT_TREE, LIGHT_TO_LEAF_IN);

// =============================================
// Bbox geometry helpers
// =============================================

// Max value of dot(dir, c - p) over the 8 bbox corners. dot is linear in c, so
// the optimum is per-axis: argmax along axis i is bMin[i] or bMax[i] by the
// sign of dir[i]. No normalization here — used as a building block.
float maxDistAlong(float3 p, float3 dir, float3 bboxMin, float3 bboxMax)
{
    const float3 dirP = dir * p;
    const float3 m0 = dir * bboxMin - dirP;
    const float3 m1 = dir * bboxMax - dirP;
    return max(m0.x, m1.x) + max(m0.y, m1.y) + max(m0.z, m1.z);
}

// min |dot(dir, c - p)| over the 8 bbox corners. Returns 0 when the corners
// straddle the plane through p perpendicular to dir.
float absMinDistAlong(float3 p, float3 dir, float3 bboxMin, float3 bboxMax)
{
    const float a = dot(dir, float3(bboxMin.x, bboxMin.y, bboxMin.z) - p);
    const float b = dot(dir, float3(bboxMin.x, bboxMin.y, bboxMax.z) - p);
    const float c = dot(dir, float3(bboxMin.x, bboxMax.y, bboxMin.z) - p);
    const float d = dot(dir, float3(bboxMin.x, bboxMax.y, bboxMax.z) - p);
    const float e = dot(dir, float3(bboxMax.x, bboxMin.y, bboxMin.z) - p);
    const float f = dot(dir, float3(bboxMax.x, bboxMin.y, bboxMax.z) - p);
    const float g = dot(dir, float3(bboxMax.x, bboxMax.y, bboxMin.z) - p);
    const float h = dot(dir, float3(bboxMax.x, bboxMax.y, bboxMax.z) - p);
    const bool hasPos = (a > 0.f) || (b > 0.f) || (c > 0.f) || (d > 0.f)
                     || (e > 0.f) || (f > 0.f) || (g > 0.f) || (h > 0.f);
    const bool hasNeg = (a < 0.f) || (b < 0.f) || (c < 0.f) || (d < 0.f)
                     || (e < 0.f) || (f < 0.f) || (g < 0.f) || (h < 0.f);
    if (hasPos && hasNeg)
    {
        return 0.0f;
    }
    return min(min(min(abs(a), abs(b)), min(abs(c), abs(d))),
               min(min(abs(e), abs(f)), min(abs(g), abs(h))));
}

// Upper bound on max{ω in cone from x to bbox} dot(N, ω). True bbox-interior
// bound (RTSL ref impl, Lin & Yuksel 2020): project bbox onto a tangent frame
// (T, B) of N; nrm_max is max along N, (y_amin, z_amin) is the closest
// in-plane offset. cos angle is bounded by nrm_max / sqrt(nrm_max² + y_amin² +
// z_amin²). Tighter and more correct than 8-corner enumeration (corners do
// not cover bbox-interior maxima).
float geomTermBound(float3 p, float3 N, float3 bboxMin, float3 bboxMax)
{
    const float nrmMax = maxDistAlong(p, N, bboxMin, bboxMax);
    if (nrmMax <= 0.0f)
    {
        return 0.0f;
    }
    float3 T;
    if (abs(N.x) > abs(N.y))
    {
        T = float3(-N.z, 0.0f, N.x) * rsqrt(N.x * N.x + N.z * N.z);
    }
    else
    {
        T = float3(0.0f, N.z, -N.y) * rsqrt(N.y * N.y + N.z * N.z);
    }
    const float3 B = normalize(cross(N, T));
    const float yAmin = absMinDistAlong(p, T, bboxMin, bboxMax);
    const float zAmin = absMinDistAlong(p, B, bboxMin, bboxMax);
    const float hyp2 = yAmin * yAmin + zAmin * zAmin + nrmMax * nrmMax;
    return nrmMax * rsqrt(hyp2);
}

// Squared distance from x to bbox (closest-point, 0 if x inside) and to the
// farthest bbox corner.
void distanceSquaredToBbox(float3 x, float3 bboxMin, float3 bboxMax,
                            out float dMinSq, out float dMaxSq)
{
    const float3 closest = x - clamp(x, bboxMin, bboxMax);
    dMinSq = dot(closest, closest);
    const float3 farthest = max(abs(x - bboxMin), abs(x - bboxMax));
    dMaxSq = dot(farthest, farthest);
}

// =============================================
// HIS weight helpers
// =============================================

// Child probability mix per RTSL paper: p_j = 0.5 * (p_j^min + p_j^max), where
// p_j^min = w_j^min / Σ w^min with w_j^min = F_j · I_j / d_j^min². Sets
// p1 + p2 == 0 when both children are dead AND allowPruning is true (caller
// bails). When allowPruning is false, falls back to uniform 0.5/0.5 so the
// descent continues — used when the brdfBound is incomplete (e.g. material has
// a glossy lobe we cannot bound yet). See plan.md §scope.
//
// Edge case (paper §3.2 last paragraph): when x is inside BOTH children's
// bboxes, d_min² collapses to 0 for both → 1/0. Drop the 1/d_min² term from
// w^min entirely; the ratio falls back to F·I-only and stays well-defined.
void rtslChildProbs(LightTreeNode c1,
                    LightTreeNode c2,
                    float3 hitPos,
                    float3 hitNormal,
                    float3 brdfBound,
                    bool allowPruning,
                    out float p1,
                    out float p2)
{
    const float lum = luminance(brdfBound);
    const float core1 = (c1.flux > 0.0f) ? (lum * geomTermBound(hitPos, hitNormal, c1.bboxMin, c1.bboxMax) * c1.flux) : 0.0f;
    const float core2 = (c2.flux > 0.0f) ? (lum * geomTermBound(hitPos, hitNormal, c2.bboxMin, c2.bboxMax) * c2.flux) : 0.0f;

    float dMinSq1, dMaxSq1, dMinSq2, dMaxSq2;
    distanceSquaredToBbox(hitPos, c1.bboxMin, c1.bboxMax, dMinSq1, dMaxSq1);
    distanceSquaredToBbox(hitPos, c2.bboxMin, c2.bboxMax, dMinSq2, dMaxSq2);

    const bool dropDistanceFromMin = (dMinSq1 == 0.0f) && (dMinSq2 == 0.0f);
    const float wMin1 = dropDistanceFromMin ? core1 : (core1 / max(dMinSq1, 1e-20f));
    const float wMin2 = dropDistanceFromMin ? core2 : (core2 / max(dMinSq2, 1e-20f));
    const float wMax1 = (dMaxSq1 > 0.0f) ? (core1 / dMaxSq1) : core1;
    const float wMax2 = (dMaxSq2 > 0.0f) ? (core2 / dMaxSq2) : core2;

    const float sumMin = wMin1 + wMin2;
    const float sumMax = wMax1 + wMax2;

    if (sumMin == 0.0f && sumMax == 0.0f)
    {
        // Both children's diffuse bound collapses to zero. With a complete
        // bound this is a true dead branch (geometric back-face). With an
        // incomplete bound (glossy lobe present) we cannot rule the subtree
        // out — descend uniformly so the glossy contribution is still
        // reachable. MIS recovery must mirror this choice.
        p1 = allowPruning ? 0.0f : 0.5f;
        p2 = allowPruning ? 0.0f : 0.5f;
        return;
    }

    const float pMin1 = (sumMin > 0.0f) ? (wMin1 / sumMin) : 0.0f;
    const float pMax1 = (sumMax > 0.0f) ? (wMax1 / sumMax) : 0.0f;

    p1 = 0.5f * (pMin1 + pMax1);
    p2 = 1.0f - p1;
}

// =============================================
// Forward sampler — HIS descent from a subtree root
// =============================================

// Returns false (with areaLightIdx = LIGHT_IDX_INVALID, pdfSelect = 0) when:
//   * the light tree is empty (no area lights this frame)
//   * the descent hits a dead branch (both children have w == 0)
//   * the descent terminates at a sentinel/padding leaf
// The caller still counts a "null light" as one sample (no shadow ray, no
// contribution). Do NOT retry — retrying introduces bias (offline paper §3.2.2).
bool selectLightFromSubtree(uint subtreeRoot,
                            float3 hitPos,
                            float3 hitNormal,
                            float3 brdfBound,
                            bool allowPruning,
                            inout RandomNumberGenerator rng,
                            out uint areaLightIdx,
                            out float pdfSelect)
{
    areaLightIdx = LIGHT_IDX_INVALID;
    pdfSelect = 0.0f;

    if (rtslParams.treeLeafCount == 0u)
    {
        return false;
    }

    float r = rng.nextFloat();
    float pdf = 1.0f;
    uint cur = subtreeRoot;

    // Descend until cur is a leaf (leaf indices start at rtslParams.treeLeafBase).
    [loop]
    while (cur < rtslParams.treeLeafBase)
    {
        const uint leftIdx = 2u * cur + 1u;
        const uint rightIdx = 2u * cur + 2u;

        const LightTreeNode leftNode = rtslLightTree[leftIdx];
        const LightTreeNode rightNode = rtslLightTree[rightIdx];

        float p1, p2;
        rtslChildProbs(leftNode, rightNode, hitPos, hitNormal, brdfBound, allowPruning, p1, p2);

        if (p1 + p2 <= 0.0f)
        {
            return false; // dead branch — null light
        }

        if (r < p1)
        {
            pdf *= p1;
            r = (p1 > 0.0f) ? (r / p1) : r;
            cur = leftIdx;
        }
        else
        {
            pdf *= p2;
            r = (p2 > 0.0f) ? ((r - p1) / p2) : r;
            cur = rightIdx;
        }
    }

    const LightTreeNode leaf = rtslLightTree[cur];
    if (leaf.areaLightIdx == LIGHT_IDX_INVALID)
    {
        return false; // sentinel padding leaf
    }

    areaLightIdx = leaf.areaLightIdx;
    pdfSelect = pdf;
    return true;
}

// =============================================
// MIS pdf recovery — root→leaf walk using the leaf's index bits as the path
// =============================================

// Returns the probability the forward sampler would assign to `areaLightIdx`
// at this shading point. Used by the BSDF-hit emission MIS branch.
// Must use the IDENTICAL weight formula as selectLightFromSubtree at each
// internal node — see rtslChildProbs.
float evaluateLightSelectPdf(uint areaLightIdx, float3 hitPos, float3 hitNormal, float3 brdfBound, bool allowPruning)
{
    if (rtslParams.treeLeafCount == 0u)
    {
        return 0.0f;
    }

    const uint leafIdx = rtslLightToLeaf[areaLightIdx];
    if (leafIdx == LEAF_IDX_INVALID)
    {
        return 0.0f;
    }

    const uint pathOffset = leafIdx - rtslParams.treeLeafBase; // 0 .. M-1
    // log2(M) — M is pow2, so firstbithigh gives the MSB index, which equals
    // the path bit count.
    const uint logM = firstbithigh(rtslParams.treeLeafCount);

    uint cur = 0u;
    float pdf = 1.0f;

    [loop]
    for (uint d = 0u; d < logM; ++d)
    {
        const uint leftIdx = 2u * cur + 1u;
        const uint rightIdx = 2u * cur + 2u;

        const LightTreeNode leftNode = rtslLightTree[leftIdx];
        const LightTreeNode rightNode = rtslLightTree[rightIdx];

        float p1, p2;
        rtslChildProbs(leftNode, rightNode, hitPos, hitNormal, brdfBound, allowPruning, p1, p2);

        if (p1 + p2 <= 0.0f)
        {
            return 0.0f; // dead ancestor
        }

        // Bit (logM - 1 - d) of pathOffset: MSB first.
        const uint bit = (pathOffset >> (logM - 1u - d)) & 1u;
        if (bit == 0u)
        {
            pdf *= p1;
            cur = leftIdx;
        }
        else
        {
            pdf *= p2;
            cur = rightIdx;
        }
    }

    return pdf;
}

#endif // LIGHT_TREE_SAMPLING_HLSLI
