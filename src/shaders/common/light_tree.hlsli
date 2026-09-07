// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#include "util/packing.hlsli"

// =============================================
// Tree node helpers
// =============================================

// Bounds are stored as f16 rounded outward (min toward -inf, max toward +inf) so the
// stored box always contains the exact one. The geometric bound prunes a subtree with
// zero pdf when its whole box is behind the shading plane, so a box that shrank could
// lose a light that sits barely in front of it; a box that only grows costs a little
// importance and no bias. Internal nodes union values that are already representable
// halves, so only leaves actually round.
LightTreeNode makeLightTreeNode(const float3 bboxMin, const float3 bboxMax, const float flux)
{
    LightTreeNode n;
    n.packedBboxMinXY = f32tof16RoundDown(bboxMin.x) | (f32tof16RoundDown(bboxMin.y) << 16);
    n.packedBboxMinZMaxX = f32tof16RoundDown(bboxMin.z) | (f32tof16RoundUp(bboxMax.x) << 16);
    n.packedBboxMaxYZ = f32tof16RoundUp(bboxMax.y) | (f32tof16RoundUp(bboxMax.z) << 16);
    n.flux = flux;
    return n;
}

void getLightTreeBbox(const LightTreeNode n, out float3 bboxMin, out float3 bboxMax)
{
    const float2 minXY = unpackUintToFloat2(n.packedBboxMinXY);
    const float2 minZMaxX = unpackUintToFloat2(n.packedBboxMinZMaxX);
    const float2 maxYZ = unpackUintToFloat2(n.packedBboxMaxYZ);
    bboxMin = float3(minXY, minZMaxX.x);
    bboxMax = float3(minZMaxX.y, maxYZ);
}

// Inverted-infinity bbox + flux=0. Unions cleanly through the bottom-up
// internal-levels pass without a "is this slot live" branch.
LightTreeNode makeSentinelLightTreeNode()
{
    const float posInf = asfloat(0x7F800000u);
    const float negInf = asfloat(0xFF800000u);
    return makeLightTreeNode(float3(posInf, posInf, posInf), float3(negInf, negInf, negInf), 0.0f);
}

LightTreeNode unionLightTreeNodes(LightTreeNode a, LightTreeNode b)
{
    float3 bboxMinA, bboxMaxA, bboxMinB, bboxMaxB;
    getLightTreeBbox(a, bboxMinA, bboxMaxA);
    getLightTreeBbox(b, bboxMinB, bboxMaxB);
    return makeLightTreeNode(min(bboxMinA, bboxMinB), max(bboxMaxA, bboxMaxB), a.flux + b.flux);
}

// =============================================
// Morton code (30-bit, 10 bits per axis)
// =============================================

// Standard "expand 10 bits into 30 by inserting 2 zero bits between each"
// shift-XOR trick. Output occupies bit positions 0, 3, 6, ..., 27.
uint expandBits10(uint v)
{
    v = (v ^ (v << 16)) & 0xFF0000FFu;
    v = (v ^ (v <<  8)) & 0x0300F00Fu;
    v = (v ^ (v <<  4)) & 0x030C30C3u;
    v = (v ^ (v <<  2)) & 0x09249249u;
    return v;
}

// Interleaves the low 10 bits of x/y/z into a 30-bit Morton code. Top 2 bits
// of the returned uint are zero so the result sorts cleanly as an ascending
// uint32 radix key.
uint morton30(uint3 q)
{
    return (expandBits10(q.z) << 2) | (expandBits10(q.y) << 1) | expandBits10(q.x);
}

// 'centroid' is reserved as an HLSL interpolation modifier — use a different
// parameter name.
uint mortonEncode30(float3 pos, float3 sceneMin, float3 sceneMax)
{
    const float3 extent = max(sceneMax - sceneMin, float3(1e-30f, 1e-30f, 1e-30f));
    const float3 t = saturate((pos - sceneMin) / extent);
    const uint3 q = min(uint3(t * 1024.0f), uint3(1023u, 1023u, 1023u));
    return morton30(q);
}

// =============================================
// Atomic float min/max via IEEE-monotonic uint encoding
// =============================================

// Map float to uint such that uint-compare gives the same order as float-compare:
//   non-negative: set top bit  (positives sort above negatives)
//   negative:     invert all bits (larger-magnitude negatives sort lowest)
// Reversible via orderableUintToFloat.
uint floatToOrderableUint(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

float orderableUintToFloat(uint u)
{
    const uint v = (u & 0x80000000u) ? (u & 0x7FFFFFFFu) : ~u;
    return asfloat(v);
}

void atomicMinFloat(RWByteAddressBuffer buf, uint offsetBytes, float val)
{
    uint dummy;
    buf.InterlockedMin(offsetBytes, floatToOrderableUint(val), dummy);
}

void atomicMaxFloat(RWByteAddressBuffer buf, uint offsetBytes, float val)
{
    uint dummy;
    buf.InterlockedMax(offsetBytes, floatToOrderableUint(val), dummy);
}

// =============================================
// dev_sceneBbox layout (RWByteAddressBuffer, 24 B)
// =============================================
//   offset  0/4/8:    min  x/y/z as orderableUint(float)
//   offset 12/16/20:  max  x/y/z as orderableUint(float)

#define SCENE_BBOX_MIN_OFFSET_BYTES 0u
#define SCENE_BBOX_MAX_OFFSET_BYTES 12u

float3 loadSceneBboxMin(RWByteAddressBuffer buf)
{
    const uint3 u = buf.Load3(SCENE_BBOX_MIN_OFFSET_BYTES);
    return float3(orderableUintToFloat(u.x), orderableUintToFloat(u.y), orderableUintToFloat(u.z));
}

float3 loadSceneBboxMax(RWByteAddressBuffer buf)
{
    const uint3 u = buf.Load3(SCENE_BBOX_MAX_OFFSET_BYTES);
    return float3(orderableUintToFloat(u.x), orderableUintToFloat(u.y), orderableUintToFloat(u.z));
}
