// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once
#ifndef COMMON_STRUCTS_H
#define COMMON_STRUCTS_H

#ifdef __cplusplus
#include <DirectXMath.h>

#define int3 DirectX::XMINT3

#define uint uint32_t
#define uint2 DirectX::XMUINT2

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3

#define float4x4 DirectX::XMFLOAT4X4
#endif

struct HitInfo
{
    float3 hitPos_WS;
    uint instanceId;

    float3 hitShadingNor_WS; // TODO: pack this?
    uint triangleIdx;

    float2 uv;
    uint packedGeoNor; // face-oriented geometric normal for surface ray offsets
    uint pad0;
};

struct GbufferData
{
    HitInfo hitInfo;

    uint materialIdx;
    uint payloadFlags;
    uint pad0;
    uint pad1;
};

struct Vertex
{
    float3 pos_OS;
    uint packedNor; // octahedron-encoded, see packing.hlsli / util/packing.h
    float2 uv; // full precision: f16 UVs can shift samples by a texel on 2K normal maps
};

struct VertexTangent
{
    uint packedTangent; // octahedron-encoded object-space direction
    float handedness; // glTF tangent.w
};

// Fixed-point position encoding of PackedTerrainVertex: (pos + bias) * scale stored as u16. The
// scales are powers of two so block corners and 1/8 liquid tops decode exactly; model geometry
// and jitter round to the grid, and the fp32 copy the BLAS is built from is decoded from the
// packed form so the traced and shaded surfaces agree
#define PACKED_TERRAIN_POS_XZ_SCALE 1024.f
#define PACKED_TERRAIN_POS_XZ_BIAS 8.f
#define PACKED_TERRAIN_POS_Y_SCALE 64.f
#define PACKED_TERRAIN_POS_Y_BIAS 1.f

// Resident form of terrain vertices, read only by shaders: the BLAS is built from the fp32 Vertex
// staging upload, so this layout is free of DXR's vertex format rules
struct PackedTerrainVertex
{
    uint packedPosXY; // x in the low half, y in the high half
    uint packedPosZUv; // z in the low half, uv as unorm8x2 in the high half
    uint packedNor; // as Vertex::packedNor
};

#define VERTEX_FORMAT_FULL 0
#define VERTEX_FORMAT_PACKED_TERRAIN 1

#define TANGENT_BUFFER_OFFSET_INVALID ~0u

struct InstanceData
{
    uint vertsBufferOffset;
    uint hasIdxs;
    uint idxsBufferByteOffset;
    uint perFaceDatasBufferOffset;

    int3 transformOffset;
    uint areaLightsBufferOffset;

    uint materialIdx;
    uint tangentsBufferOffset; // separate VertexTangent array, or TANGENT_BUFFER_OFFSET_INVALID
    uint trisPerFaceLog2; // triangle index >> this = PerFaceData index; 0 for glTF, 1 for terrain quads
    uint vertexFormat; // VERTEX_FORMAT_*, selects which typed view of the verts buffer to read
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
// Per-material, not per-texture: all texture slots must be Texture2DArray (or invalid).
#define MATERIAL_FLAG_ARRAY_TEXTURE (1 << 3)
// auxTextureId is a packed aux texture: r = emissive strength (color comes from the
// base color texture, whose diffuse is zero wherever r > 0), g = biome tint mask.
#define MATERIAL_FLAG_PACKED_AUX (1 << 4)

#define MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION (MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_GLOSSY_TRANSMISSION)
#define MATERIAL_FLAGS_GLOSSY (MATERIAL_FLAG_GLOSSY_REFLECTION | MATERIAL_FLAG_GLOSSY_TRANSMISSION)
// The lobe bits, as opposed to the texture bits, so a path split can swap lobes without touching the rest
#define MATERIAL_FLAGS_LOBES (MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_GLOSSY_REFLECTION | MATERIAL_FLAG_GLOSSY_TRANSMISSION)

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

    uint roughnessTextureId; // linear glTF metallicRoughnessTexture; only G is used
    uint normalTextureId; // linear tangent-space normal, separate for both terrain and glTF
    float normalScale; // scales normal texture X/Y before normalization
    uint pad0;

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
// children of node i at 2i+1 and 2i+2). Bounds are f16 pairs rounded outward,
// see makeLightTreeNode() in light_tree.hlsli; 16 bytes so both children of a
// node come from one 32-byte load. Leaves carry no light index: leaf offset s
// maps to its sparse AreaLight[] index through the sorted morton values buffer.
struct LightTreeNode
{
    uint packedBboxMinXY;
    uint packedBboxMinZMaxX;
    uint packedBboxMaxYZ;
    float flux;
};

#ifdef __cplusplus
static_assert(sizeof(LightAux) == 32, "LightAux must be 32 bytes for parity with the HLSL StructuredBuffer<LightAux> layout");
static_assert(sizeof(LightTreeNode) == 16, "LightTreeNode must be 16 bytes for parity with the HLSL StructuredBuffer<LightTreeNode> layout");
static_assert(sizeof(Vertex) == 24, "Vertex must be 24 bytes for parity with the HLSL StructuredBuffer<Vertex> layout");
static_assert(sizeof(PackedTerrainVertex) == 12, "PackedTerrainVertex must be 12 bytes for parity with the HLSL layout");
#endif

#define FACE_FLAG_IS_WATER (1 << 0)
// Faces that receive wave displacement and noise-based normals perturbation
#define FACE_FLAG_IS_WATER_TOP (1 << 1)
// Faces whose base color is replaced by luminance * biome map tint
#define FACE_FLAG_BIOME_TINT (1 << 2)
// Foliage faces with thin-wall diffuse transmission: diffuse splits into reflection and transmission
#define FACE_FLAG_DIFFUSE_TRANSMISSION (1 << 3)
// Faces shaded as glass: the terrain material's diffuse lobe is replaced by glossy reflection +
// transmission, with per-texel roughness from the packed aux b channel (see applyGlassMaterial)
#define FACE_FLAG_IS_GLASS (1 << 4)
// Faces whose base and emissive color come from a world-space ramp (see getProceduralColor)
#define FACE_FLAG_PROCEDURAL_COLOR (1 << 5)
// The terrain texture array slice has a normal map.
#define FACE_FLAG_NORMAL_MAP (1 << 6)
// The terrain texture array slice has at least one per-texel mask bit set in packed aux alpha
// (AUX_MASK_*), so hits must decode it (see applyAuxMasks)
#define FACE_FLAG_AUX_MASKS (1 << 7)

// Per-texel binary masks packed as bits into the terrain aux alpha channel at startup. Each is
// authored as its own <texture>.<suffix>.png (see TerrainMaterials::auxMaskDefs). Voxel mode point
// samples, so a sample always reads one texel's bits intact; mips are built per bit.
// Texels shaded as diffuse with a glossy reflection coat on top, roughness from aux b
#define AUX_MASK_GLOSSY (1u << 0)

#define FACE_FLAGS_BITS 16
#define FACE_FLAGS_MASK ((1u << FACE_FLAGS_BITS) - 1u)

// One entry per mesh face: a triangle for glTF instances, a quad (triangle pair) for terrain.
// See knowledge/scene/instance.md for the area light invariant this relies on.
struct PerFaceData
{
#ifdef __cplusplus
public:
    PerFaceData();
    void setFlags(uint32_t flags);
    void setTexArraySliceIdx(uint32_t sliceIdx);
#endif

    uint packedFlagsAndSlice; // bits 0-15 FACE_FLAG_*, bits 16-31 texture array slice
    uint localAreaLightIdx; // of the face's first triangle, or LIGHT_IDX_INVALID

    uint getFlags()
    {
        return packedFlagsAndSlice & FACE_FLAGS_MASK;
    }

    bool hasFlag(uint flag)
    {
        return bool(packedFlagsAndSlice & flag);
    }

    uint getTexArraySliceIdx()
    {
        return packedFlagsAndSlice >> FACE_FLAGS_BITS;
    }
};

#ifdef __cplusplus
static_assert(sizeof(PerFaceData) == 8, "PerFaceData must be 8 bytes for parity with the HLSL layout");
#endif

#ifdef __cplusplus
#undef int3

#undef uint
#undef uint2

#undef float2
#undef float3

#undef float4x4
#endif

#endif // COMMON_STRUCTS_H
