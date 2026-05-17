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

// Returns max( dot(n, normalize(corner - x)) ) over the 8 bbox corners. Trying
// all 8 (rather than the axis-picked corner that maximizes the UNNORMALIZED
// dot) is necessary because normalization can change the ordering. If any
// corner coincides with x (degenerate, x exactly on a corner), returns 1.
float maxDotToBbox(float3 n, float3 x, float3 bboxMin, float3 bboxMax)
{
    float bestDot = 0.0f;
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        const float3 corner = float3(
            (i & 1u) ? bboxMax.x : bboxMin.x,
            (i & 2u) ? bboxMax.y : bboxMin.y,
            (i & 4u) ? bboxMax.z : bboxMin.z);
        const float3 v = corner - x;
        const float lenSq = dot(v, v);
        if (lenSq < 1e-30f)
        {
            return 1.0f;
        }
        bestDot = max(bestDot, dot(n, v * rsqrt(lenSq)));
    }
    return bestDot;
}

// Bounding-sphere distance bounds. Smoother than the per-corner bbox bounds
// (which have a discontinuity at the bbox boundary and a 1/eps singularity
// for x inside the bbox), so adjacent pixels straddling a cluster boundary
// don't see wildly different importance weights.
//
// dMin is regularized to at least half the sphere radius so it never goes to
// zero (or negative when x is inside the sphere). dMax is always positive.
void distanceBoundsToSphere(float3 x, float3 bboxMin, float3 bboxMax, out float dMinSq, out float dMaxSq)
{
    const float3 center = 0.5f * (bboxMin + bboxMax);
    const float radius = 0.5f * length(bboxMax - bboxMin);
    const float dCenter = length(center - x);

    const float dMin = max(dCenter - radius, 0.5f * max(radius, 1e-10f));
    const float dMax = dCenter + radius;

    dMinSq = dMin * dMin;
    dMaxSq = dMax * dMax;
}

// =============================================
// HIS weight helpers
// =============================================

void rtslChildWeightsForNode(LightTreeNode child,
                             float3 hitPos,
                             float3 hitNormal,
                             float3 brdfBound,
                             out float wMin,
                             out float wMax)
{
    if (child.flux <= 0.0f)
    {
        wMin = 0.0f;
        wMax = 0.0f;
        return;
    }

    float dMinSq, dMaxSq;
    distanceBoundsToSphere(hitPos, child.bboxMin, child.bboxMax, dMinSq, dMaxSq);

    const float reflectance = luminance(brdfBound) * maxDotToBbox(hitNormal, hitPos, child.bboxMin, child.bboxMax);
    const float weightCore = reflectance * child.flux;

    wMin = weightCore / dMinSq;
    wMax = weightCore / dMaxSq;
}

// Child probability mix per RTSL paper: p = 0.5 * (p_min + p_max).
// Sets p1 + p2 == 0 when the entire subtree is a dead branch (caller bails).
void rtslChildProbs(LightTreeNode c1,
                    LightTreeNode c2,
                    float3 hitPos,
                    float3 hitNormal,
                    float3 brdfBound,
                    out float p1,
                    out float p2)
{
    float wMin1, wMax1, wMin2, wMax2;
    rtslChildWeightsForNode(c1, hitPos, hitNormal, brdfBound, wMin1, wMax1);
    rtslChildWeightsForNode(c2, hitPos, hitNormal, brdfBound, wMin2, wMax2);

    const float sumMin = wMin1 + wMin2;
    const float sumMax = wMax1 + wMax2;

    const float pMin1 = (sumMin > 0.0f) ? (wMin1 / sumMin) : 0.0f;
    const float pMax1 = (sumMax > 0.0f) ? (wMax1 / sumMax) : 0.0f;

    if (sumMin == 0.0f && sumMax == 0.0f)
    {
        // Dead branch.
        p1 = 0.0f;
        p2 = 0.0f;
        return;
    }

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
        rtslChildProbs(leftNode, rightNode, hitPos, hitNormal, brdfBound, p1, p2);

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
float evaluateLightSelectPdf(uint areaLightIdx, float3 hitPos, float3 hitNormal, float3 brdfBound)
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
        rtslChildProbs(leftNode, rightNode, hitPos, hitNormal, brdfBound, p1, p2);

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
