// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

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

    float3 hitNor_WS;
    uint triangleIdx;

    float2 uv;
    uint pad0;
    uint pad1;
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
    float3 nor; // TODO: pack into one uint using octEncode
    float2 uv; // TODO: pack into one uint?
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
    uint pad0;
    uint pad1;
    uint pad2;
};

#define MATERIAL_IDX_INVALID ~0u
#define TEXTURE_ID_INVALID ~0u
#define LIGHT_IDX_INVALID ~0u

#define MATERIAL_FLAG_DIFFUSE (1 << 0)
#define MATERIAL_FLAG_GLOSSY_REFLECTION (1 << 1) // glossy includes specular (roughness = 0) and glossy (roughness > 0)
#define MATERIAL_FLAG_GLOSSY_TRANSMISSION (1 << 2)

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
    uint pad2;
    uint pad3;

    float3 baseColor;
    uint baseColorTextureId;

    float3 glossyReflectionTint;
    float ior;

    float3 emissiveColor;
    uint emissiveColorTextureId;

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

    bool isDelta()
    {
        return (flags & MATERIAL_FLAGS_GLOSSY) && !(flags & MATERIAL_FLAG_DIFFUSE); // TODO: update after adding roughness
    }

    bool hasDiffuseOrGlossyTransmission()
    {
        return bool(flags & MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION);
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

#define TRIANGLE_FLAG_IS_WATER (1 << 0)

struct PerTriangleData
{
#ifdef __cplusplus
public:
    PerTriangleData();
#endif

    uint flags;
    uint localAreaLightIdx;
    uint pad0;
    uint pad1;
};

#ifdef __cplusplus
#undef int3

#undef uint
#undef uint2

#undef float2
#undef float3

#undef float4x4
#endif
