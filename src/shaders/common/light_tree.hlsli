// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#ifndef LIGHT_TREE_HLSLI
#define LIGHT_TREE_HLSLI

#include "../../rendering/common/common_structs.h"

// =============================================
// Tree node helpers
// =============================================

// Inverted-infinity bbox + flux=0 + invalid idx. Unions cleanly through the
// bottom-up internal-levels pass without a "is this slot live" branch.
LightTreeNode makeSentinelLightTreeNode()
{
    const float posInf = asfloat(0x7F800000u);
    const float negInf = asfloat(0xFF800000u);
    LightTreeNode n;
    n.bboxMin = float3(posInf, posInf, posInf);
    n.flux = 0.0f;
    n.bboxMax = float3(negInf, negInf, negInf);
    n.areaLightIdx = LIGHT_IDX_INVALID;
    return n;
}

LightTreeNode unionLightTreeNodes(LightTreeNode a, LightTreeNode b)
{
    LightTreeNode n;
    n.bboxMin = min(a.bboxMin, b.bboxMin);
    n.flux = a.flux + b.flux;
    n.bboxMax = max(a.bboxMax, b.bboxMax);
    n.areaLightIdx = LIGHT_IDX_INVALID;
    return n;
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

#endif // LIGHT_TREE_HLSLI
