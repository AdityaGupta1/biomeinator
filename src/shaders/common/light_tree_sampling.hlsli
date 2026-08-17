// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// Real-Time Stochastic Lightcuts (Lin & Yuksel 2020) HIS sampler + MIS pdf
// recovery. Reads the perfect-binary tree built by LightTreeManager
// (Stage 1/2 of plans/plan.md). Per-pixel root descent — no cut sharing yet
// (Stage 5 lands that).

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/light_tree.hlsli"
#include "light/light_sampling.hlsli"
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

// Thin-translucent surfaces scatter into both hemispheres, so their bound must consider the
// flipped normal too — otherwise a subtree entirely behind the surface plane would be pruned
// even though transmission can still reach it.
float geomTermBoundTwoSided(float3 p, float3 N, bool isTranslucent, float3 bboxMin, float3 bboxMax)
{
    float bound = geomTermBound(p, N, bboxMin, bboxMax);
    if (isTranslucent)
    {
        bound = max(bound, geomTermBound(p, -N, bboxMin, bboxMax));
    }
    return bound;
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

// Child probability mix per RTSL paper: p_j = 0.5 * (p_j^min + p_j^max), with
// w_j = I_j · G_j / d_j². The paper's F factor (reflectance bound) is omitted
// here; it is the same scalar for both children after luminance reduction and
// therefore cancels in p_j. Matches the reference impl (SLCHelperFunctions.hlsli
// :: firstChildWeight uses pure I · G). Dropping F also removes the glossy-only
// degenerate case where F = 0 collapses the descent to uniform.
//
// Dead-branch detection is purely geometric: core == 0 ⟺ flux == 0 (padding
// leaf / empty subtree) or the geometric bound == 0 (entire bbox back-facing
// wrt N; translucent hits also check -N, so no reachable light path exists).
// Both cases prune safely.
//
// Per-child ratios use the cross-multiplied form
// (core1 · d_j^2) / (core1 · d_j^2 + core2 · d_i^2) instead of computing
// (core/d²) explicitly — algebraically equivalent but avoids dividing by
// tiny d² values (matches reference impl's `normalizedWeights`).
//
// Edge case (paper §3.2 last paragraph): when x is inside BOTH children's
// bboxes, d_min² collapses to 0 for both → both cross-multiplied terms vanish.
// Drop the distance term entirely; the ratio falls back to core-only.
void rtslChildProbs(LightTreeNode c1,
                    LightTreeNode c2,
                    float3 hitPos,
                    float3 hitNormal,
                    bool isTranslucent,
                    out float p1,
                    out float p2)
{
    const float core1 = (c1.flux > 0.0f) ? (geomTermBoundTwoSided(hitPos, hitNormal, isTranslucent, c1.bboxMin, c1.bboxMax) * c1.flux) : 0.0f;
    const float core2 = (c2.flux > 0.0f) ? (geomTermBoundTwoSided(hitPos, hitNormal, isTranslucent, c2.bboxMin, c2.bboxMax) * c2.flux) : 0.0f;

    if (core1 == 0.0f && core2 == 0.0f)
    {
        p1 = 0.0f;
        p2 = 0.0f;
        return;
    }
    if (core1 == 0.0f)
    {
        p1 = 0.0f;
        p2 = 1.0f;
        return;
    }
    if (core2 == 0.0f)
    {
        p1 = 1.0f;
        p2 = 0.0f;
        return;
    }

    float dMinSq1, dMaxSq1, dMinSq2, dMaxSq2;
    distanceSquaredToBbox(hitPos, c1.bboxMin, c1.bboxMax, dMinSq1, dMaxSq1);
    distanceSquaredToBbox(hitPos, c2.bboxMin, c2.bboxMax, dMinSq2, dMaxSq2);

    // Both children's core > 0 past this point, so the cross-multiplied
    // denominators are strictly positive whenever at least one distance > 0.
    const bool dropDistanceFromMin = (dMinSq1 == 0.0f) && (dMinSq2 == 0.0f);
    const float pMin1 = dropDistanceFromMin
        ? (core1 / (core1 + core2))
        : ((core1 * dMinSq2) / (core1 * dMinSq2 + core2 * dMinSq1));

    // dMaxSq is zero only if the bbox degenerates to the shading point — same
    // fallback as min for robustness.
    const bool dropDistanceFromMax = (dMaxSq1 == 0.0f) && (dMaxSq2 == 0.0f);
    const float pMax1 = dropDistanceFromMax
        ? (core1 / (core1 + core2))
        : ((core1 * dMaxSq2) / (core1 * dMaxSq2 + core2 * dMaxSq1));

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
                            bool isTranslucent,
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

    // Leaf indices start at rtslParams.treeLeafBase.
    [loop]
    while (cur < rtslParams.treeLeafBase)
    {
        const uint leftIdx = 2u * cur + 1u;
        const uint rightIdx = 2u * cur + 2u;

        const LightTreeNode leftNode = rtslLightTree[leftIdx];
        const LightTreeNode rightNode = rtslLightTree[rightIdx];

        float p1, p2;
        rtslChildProbs(leftNode, rightNode, hitPos, hitNormal, isTranslucent, p1, p2);

        if (p1 + p2 <= 0.0f)
        {
            return false;
        }

        // p1 + p2 == 1 always (see rtslChildProbs), and the dead-branch case
        // (p1 + p2 == 0) was caught above — so the picked branch's prob is
        // always > 0 and the rescale never divides by zero. Algebraically the
        // rescale stays in [0, 1), but FP rounding at extreme p_j can land at
        // exactly 1.0; clamp below 1 so the next `r < p1` test stays correct.
        const float rMax = asfloat(0x3F7FFFFFu); // largest float < 1
        if (r < p1)
        {
            pdf *= p1;
            r = min(r / p1, rMax);
            cur = leftIdx;
        }
        else
        {
            pdf *= p2;
            r = min((r - p1) / p2, rMax);
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
float evaluateLightSelectPdf(uint areaLightIdx, float3 hitPos, float3 hitNormal, bool isTranslucent)
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
        rtslChildProbs(leftNode, rightNode, hitPos, hitNormal, isTranslucent, p1, p2);

        if (p1 + p2 <= 0.0f)
        {
            return 0.0f; // dead ancestor
        }

        // MSB first — must mirror leaf-bit construction order in selectLightFromSubtree.
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

// BSDF-hit MIS pdf recovery — mirrors lightPdfUniform's signature. Returns
// the full area-light pdf (select × area-to-solid-angle) the forward sampler
// would have assigned to the triangle that `hitInfo` landed on.
float lightPdfRtsl(const HitInfo hitInfo,
                   const float3 surfPos_WS,
                   const float3 surfNor_WS,
                   const float3 wi_WS,
                   const bool isTranslucent)
{
    const uint areaLightIdx = getAreaLightIdxFromHit(hitInfo);
    if (areaLightIdx == LIGHT_IDX_INVALID)
    {
        return 0.f;
    }

    const float pdfSelect = evaluateLightSelectPdf(areaLightIdx, surfPos_WS, surfNor_WS, isTranslucent);
    if (pdfSelect <= 0.f)
    {
        return 0.f;
    }

    const AreaLight light = areaLights[areaLightIdx];
    float3 lightNor_WS;
    float lightArea;
    getLightNormalAndArea(light, lightNor_WS, lightArea);
    const float r2 = distance2(surfPos_WS, hitInfo.hitPos_WS);
    return pdfSelect * r2 / (absCosTheta(-wi_WS, lightNor_WS) * lightArea);
}

// =============================================
// Forward NEE entry point — mirrors sampleDirectLightingUniform / evaluateRisSample
// =============================================

// One-light-sample-per-pixel RTSL NEE. Returns the picked light's contribution
// (or didHitLight=false on null sample). pdfOrW_Y is the true selection pdf
// times the area-to-solid-angle pdf — flows into the non-RIS MIS branch.
DirectLightingSample sampleDirectLightingRtsl(const float3 surfPos_WS,
                                              const float3 surfNor_WS,
                                              const RayCone rayCone,
                                              const bool canPassthrough,
                                              const bool startUnderwater,
                                              const bool isTranslucent,
                                              inout RandomNumberGenerator rng)
{
    DirectLightingSample result;
    result.didHitLight = false;

    uint pickedLightIdx;
    float pdfSelect;
    const bool gotLight = selectLightFromSubtree(
        0u, surfPos_WS, surfNor_WS, isTranslucent, rng, pickedLightIdx, pdfSelect);
    if (!gotLight)
    {
        return result;
    }

    const AreaLight light = areaLights[pickedLightIdx];

    float3 pointOnLight_WS, wi_WS;
    float2 lightBary2;
    float lightSamplePdf;
    sampleAreaLightPoint(light, surfPos_WS, rng, pointOnLight_WS, lightBary2, wi_WS, lightSamplePdf);

    float3 Le;
    const bool didHit = traceToLight(
        surfPos_WS, surfNor_WS, wi_WS, pointOnLight_WS, lightBary2, light, rayCone, canPassthrough, startUnderwater, rng, Le);
    if (!didHit)
    {
        return result;
    }

    result.lightIdx = pickedLightIdx;
    result.didHitLight = true;
    result.pointOnLight_WS = pointOnLight_WS;
    result.wi_WS = wi_WS;
    result.Le = Le;
    result.pdfOrW_Y = pdfSelect * lightSamplePdf;
    return result;
}
