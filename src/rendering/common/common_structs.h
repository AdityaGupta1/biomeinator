// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once
#ifndef COMMON_STRUCTS_H
#define COMMON_STRUCTS_H

#ifdef __cplusplus
#include <DirectXMath.h>

// These macros make the HLSL type names compile as C++ for the shared structs below. Every define
// must be matched by an #undef at the bottom of this file: a macro that leaks past it rewrites
// unrelated code in whatever gets included next (e.g. a `float4` define turns Streamline's
// `struct float4` into an attempt to redefine DirectX::XMFLOAT4 inside namespace sl).
#define int3 DirectX::XMINT3

#define uint uint32_t
#define uint2 DirectX::XMUINT2

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3

#define float3x4 DirectX::XMFLOAT3X4
#define float4x4 DirectX::XMFLOAT4X4
#define row_major
#endif

struct HitInfo
{
    float3 hitPos_WS;
    uint instanceId;

    float3 hitNor_WS;
    uint triangleIdx;

    float2 uv;
    float2 barycentrics; // of the triangle's second and third vertices, as DXR reports them
};

struct GbufferData
{
    HitInfo hitInfo;

    uint materialIdx;
    uint payloadFlags;
    uint instanceGeneration; // of hitInfo.instanceId when the hit was made, see InstanceData::generation
    uint pad0;
};

// One resampled path per pixel slot for ReSTIR PT. Holds what random replay and reconnection need to
// rebuild the path at another pixel; see knowledge/restir/design.md. Field packing is in
// shaders/restir/path_reservoir.hlsli. The reconnection vertex is stored as a mesh-relative hit and
// rebuilt on the current mesh at replay, so it follows deforming geometry.
struct PathReservoir
{
    float3 F; // integrand of the selected path, without Russian roulette division
    float W;  // unbiased contribution weight

    uint seed;
    uint flags; // path length, rc vertex, technique and lobe bits in the low half; confidence M as 8.8 fixed point in the high half
    float rcLightPdf; // light sampling pdf from the rc vertex, for MIS when the rc vertex precedes the light vertex
    float rcJacobianTerms; // product of the path's pdf and geometry terms across the reconnection, the shift Jacobian's denominator

    uint rcInstance; // instance generation << 24 | instance id; unused when the rc vertex is the dome
    uint rcTriangleIdx;
    uint rcBarycentrics; // two unorm16
    uint rcWi; // octahedral-encoded direction leaving the rc vertex toward the next vertex, or the dome direction

    float3 rcRadiance; // radiance arriving at the rc vertex along rcWi, excluding the segment before it
    uint debugFlags; // RESERVOIR_DEBUG_*, only for the debug views
};

#define RESERVOIR_DEBUG_TEMPORAL_SHIFT_SUCCEEDED (1 << 0)

// Shift outcome counters (uint per slot), written by the reuse passes when RestirParams::shiftStatsEnabled.
// A pass has RESTIR_STATS_PASS_STRIDE slots: pairs seen, pairs skipped for an empty reservoir, pairs
// skipped for a missing partner, then per replayed-vertex-count bucket (0 .. RESTIR_STATS_REPLAY_BUCKETS-1, the last bucket meaning
// that many or more) the shifts attempted and the shifts that produced a nonzero contribution.
#define RESTIR_STATS_REPLAY_BUCKETS 8
#define RESTIR_STATS_PAIRS 0
#define RESTIR_STATS_SKIPPED 1
#define RESTIR_STATS_NO_PARTNER 2
#define RESTIR_STATS_BUCKETS_BASE 3
#define RESTIR_STATS_PASS_STRIDE (RESTIR_STATS_BUCKETS_BASE + 2 * RESTIR_STATS_REPLAY_BUCKETS)
#define RESTIR_STATS_SPATIAL_BASE 0
#define RESTIR_STATS_TEMPORAL_BASE RESTIR_STATS_PASS_STRIDE
#define RESTIR_STATS_COUNT (2 * RESTIR_STATS_PASS_STRIDE)

// A neighbor's path shifted into a pixel, as produced by the spatial shift pass
struct ShiftedPath
{
    float3 F; // integrand of the shifted path at the destination pixel (0 if the shift is undefined)
    float jacobian;

    float rcJacobianTerms; // the shifted path's own terms, stored with it if resampling selects it
    uint pad0;
    uint pad1;
    uint pad2;
};

struct Vertex
{
    float3 pos_OS;
    uint packedNor; // octahedron-encoded, see packing.hlsli / util/packing.h
    uint packedUv; // f16 pair
};

struct InstanceData
{
    uint vertsBufferOffset;
    uint hasIdxs;
    uint idxsBufferByteOffset;
    uint perTriDatasBufferOffset;

    int3 transformOffset;
    uint areaLightsBufferOffset;

    uint materialIdx;
    uint generation; // incremented each time this instance id is freed, so stale references can be detected
    uint pad0;
    uint pad1;

    // The TLAS instance transform without the integer offsets (Instance::transform), so hits can be
    // rebuilt from barycentrics outside a hit shader, where ObjectToWorld() is unavailable:
    // world = objectToWorld * (pos, 1) + transformOffset - globalInstanceOffset. Declared row-major
    // so the HLSL side reads XMFLOAT3X4's memory layout as is.
    row_major float3x4 objectToWorld;
};

#define MATERIAL_IDX_INVALID ~0u
#define TEXTURE_ID_INVALID ~0u
#define LIGHT_IDX_INVALID ~0u

#define MATERIAL_FLAG_DIFFUSE (1 << 0)
#define MATERIAL_FLAG_GLOSSY_REFLECTION (1 << 1) // glossy includes specular (roughness = 0) and glossy (roughness > 0)
// Mutually exclusive with MATERIAL_FLAG_DIFFUSE: glossy transmission replaces the diffuse base lobe entirely.
// Roughness > 0 is only supported together with MATERIAL_FLAG_GLOSSY_REFLECTION (the dielectric lobe), so
// transmission-only materials are delta. Both enforced in Scene::addMaterial.
#define MATERIAL_FLAG_GLOSSY_TRANSMISSION (1 << 2)
// Per-material, not per-texture: base + aux must both be Texture2DArray (or invalid).
#define MATERIAL_FLAG_ARRAY_TEXTURE (1 << 3)
// auxTextureId is a packed aux texture: r = emissive strength (color comes from the
// base color texture, whose diffuse is zero wherever r > 0), g = biome tint mask.
#define MATERIAL_FLAG_PACKED_AUX (1 << 4)

#define MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION (MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_GLOSSY_TRANSMISSION)
#define MATERIAL_FLAGS_GLOSSY (MATERIAL_FLAG_GLOSSY_REFLECTION | MATERIAL_FLAG_GLOSSY_TRANSMISSION)

struct Material
{
#ifdef __cplusplus
public:
    Material();
#endif

    uint flags;
    float emissiveStrength;
    float diffuseTransmission; // thin-wall diffuse transmission fraction; > 0 only for thin foliage hits
    float roughness; // GGX roughness (alpha = roughness^2) for glossy reflection and transmission; 0 = perfectly specular

    float3 baseColor;
    uint baseColorTextureId;

    float3 glossyReflectionTint;
    float ior;

    float3 emissiveColor;
    uint auxTextureId; // emissive color texture, unless MATERIAL_FLAG_PACKED_AUX repurposes it

    bool hasDiffuse()
    {
        return bool(flags & MATERIAL_FLAG_DIFFUSE);
    }

    bool hasGlossyReflection()
    {
        return bool(flags & MATERIAL_FLAG_GLOSSY_REFLECTION);
    }

    bool hasGlossyTransmission()
    {
        return bool(flags & MATERIAL_FLAG_GLOSSY_TRANSMISSION);
    }

    bool hasEmission()
    {
        return emissiveStrength > 0.f;
    }

    bool hasGlossy()
    {
        return bool(flags & MATERIAL_FLAGS_GLOSSY);
    }

    bool isDelta()
    {
        return hasGlossy() && !hasDiffuse() && roughness == 0.f;
    }

    // Perfectly specular transmission is the only kind a ray can pass through instead of scattering at
    bool isDeltaTransmission()
    {
        return hasGlossyTransmission() && isDelta();
    }

    bool hasDiffuseOrGlossyTransmission()
    {
        return bool(flags & MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION);
    }

    bool hasDiffuseTransmission()
    {
        return hasDiffuse() && diffuseTransmission > 0.f;
    }

    bool hasRoughGlossyTransmission()
    {
        return hasGlossyTransmission() && roughness > 0.f;
    }

    // Light arriving from behind the shading normal can scatter towards the viewer, so light sampling
    // must consider both hemispheres
    bool acceptsBacksideLight()
    {
        return hasDiffuseTransmission() || hasRoughGlossyTransmission();
    }

    bool hasArrayTexture()
    {
        return bool(flags & MATERIAL_FLAG_ARRAY_TEXTURE);
    }

    bool hasPackedAux()
    {
        return bool(flags & MATERIAL_FLAG_PACKED_AUX);
    }

    bool canScatter()
    {
        return hasGlossyReflection() || hasDiffuseOrGlossyTransmission();
    }

#ifdef __cplusplus
    void setHasDiffuse(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_DIFFUSE) | (-uint32_t(enable) & MATERIAL_FLAG_DIFFUSE);
    }

    void setHasGlossyReflection(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_GLOSSY_REFLECTION) | (-uint32_t(enable) & MATERIAL_FLAG_GLOSSY_REFLECTION);
    }

    void setHasGlossyTransmission(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_GLOSSY_TRANSMISSION) | (-uint32_t(enable) & MATERIAL_FLAG_GLOSSY_TRANSMISSION);
    }

    void setHasArrayTexture(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_ARRAY_TEXTURE) | (-uint32_t(enable) & MATERIAL_FLAG_ARRAY_TEXTURE);
    }

    void setHasPackedAux(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_PACKED_AUX) | (-uint32_t(enable) & MATERIAL_FLAG_PACKED_AUX);
    }
#endif
};

struct AreaLight
{
    float3 pos0_WS; // not accounting for instance.transformOffset or globalInstanceOffset
    uint instanceId;

    float3 pos1_WS;
    uint triangleIdx;

    float3 pos2_WS;
    uint materialIdx;
};

#define LEAF_IDX_INVALID ~0u

// Parallel to AreaLight, keyed by the same global area light index. Holds
// per-light extras used to build the stochastic light tree (Stage 1/2).
// Bbox is world-space (already includes the instance's float transform; the
// integer transformOffset is added at shading time, same as AreaLight).
struct LightAux
{
    float3 bboxMin;
    float flux; // radiant power proxy = emissiveStrength * colorTerm * triangleArea

    float3 bboxMax;
    uint pad0;
};

// Node in the Stage 2 perfect-binary light tree (0-indexed, root at [0],
// children of node i at 2i+1 and 2i+2). Leaves hold a real `areaLightIdx`
// (sparse, into AreaLight[]); internal nodes and bogus padding-leaves use
// LIGHT_IDX_INVALID. Field order matches LightAux so HLSL/cbuffer 16-byte
// packing rules give a tight 32-byte layout with no crossed boundaries.
struct LightTreeNode
{
    float3 bboxMin;
    float flux;

    float3 bboxMax;
    uint areaLightIdx;
};

#ifdef __cplusplus
static_assert(sizeof(LightAux) == 32, "LightAux must be 32 bytes for parity with the HLSL StructuredBuffer<LightAux> layout");
static_assert(sizeof(LightTreeNode) == 32, "LightTreeNode must be 32 bytes for parity with the HLSL StructuredBuffer<LightTreeNode> layout");
#endif

#define TRIANGLE_FLAG_IS_WATER (1 << 0)
// Faces that receive wave displacement and noise-based normals perturbation
#define TRIANGLE_FLAG_IS_WATER_TOP (1 << 1)
// Faces whose base color is replaced by luminance * biome map tint
#define TRIANGLE_FLAG_BIOME_TINT (1 << 2)
// Foliage faces with thin-wall diffuse transmission: diffuse splits into reflection and transmission
#define TRIANGLE_FLAG_DIFFUSE_TRANSMISSION (1 << 3)

struct PerTriangleData
{
#ifdef __cplusplus
public:
    PerTriangleData();
#endif

    uint flags;
    uint localAreaLightIdx;
    uint texArraySliceIdx;
    uint pad0;
};

#ifdef __cplusplus
#undef int3

#undef uint
#undef uint2

#undef float2
#undef float3

#undef float3x4
#undef float4x4
#undef row_major
#endif

#endif // COMMON_STRUCTS_H
