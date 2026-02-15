/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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

#define MATERIAL_FLAG_HAS_DIFFUSE (1 << 0)
#define MATERIAL_FLAG_HAS_GLOSSY_REFLECTION (1 << 1) // glossy includes specular (roughness = 0) and glossy (roughness > 0)
#define MATERIAL_FLAG_HAS_GLOSSY_TRASNMISSION (1 << 2)

#define MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION (MATERIAL_FLAG_HAS_DIFFUSE) // TODO: add more conditions here later (e.g. specular transmission)
#define MATERIAL_FLAGS_GLOSSY_REFLECTION (MATERIAL_FLAG_HAS_GLOSSY_REFLECTION) // TODO: add more conditions here later

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

    float3 specularColor;
    float ior;

    float3 emissiveColor;
    uint emissiveColorTextureId;

    bool hasDiffuse()
    {
        return bool(flags & MATERIAL_FLAG_HAS_DIFFUSE);
    }

    bool hasSpecularReflection()
    {
        return bool(flags & MATERIAL_FLAG_HAS_GLOSSY_REFLECTION);
    }

    bool hasEmission()
    {
        return emissiveStrength > 0.f;
    }

    bool isDelta()
    {
        return (flags == MATERIAL_FLAG_HAS_GLOSSY_REFLECTION); // TODO: update after adding roughness
    }

    bool hasGlossyReflection()
    {
        return bool(flags & MATERIAL_FLAGS_GLOSSY_REFLECTION);
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
        flags = (flags & ~MATERIAL_FLAG_HAS_DIFFUSE) | (-uint32_t(enable) & MATERIAL_FLAG_HAS_DIFFUSE);
    }

    void setHasSpecularReflection(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_HAS_GLOSSY_REFLECTION) | (-uint32_t(enable) & MATERIAL_FLAG_HAS_GLOSSY_REFLECTION);
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

#define LIGHT_IDX_INVALID ~0u

struct PerTriangleData
{
#ifdef __cplusplus
public:
    PerTriangleData();
#endif

    uint localAreaLightIdx;
    uint pad0;
    uint pad1;
    uint pad2;
};

#ifdef __cplusplus
#undef int3

#undef uint
#undef uint2

#undef float2
#undef float3

#undef float4x4
#endif
