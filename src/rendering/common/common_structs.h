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

#include "common_preamble.h"

#if !_hlsl
#include <DirectXMath.h>

#define uint uint32_t

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3
#endif // !_hlsl

struct Vertex
{
    float3 pos;
    float3 nor; // TODO: pack into one? uint
    float2 uv; // TODO: pack into one uint
};

struct InstanceData
{
    uint vertBufferOffset;
    uint hasIdxs;
    uint idxBufferByteOffset;
    uint materialId;
};

#define MATERIAL_ID_INVALID ~0u
#define TEXTURE_ID_INVALID ~0u

#define MATERIAL_FLAG_HAS_DIFFUSE (1 << 0)
#define MATERIAL_FLAG_HAS_SPECULAR (1 << 1)

struct Material
{
#if !_hlsl
public:
    Material();
#endif

    uint flags;
    uint pad1;
    uint pad2;
    uint pad3;

    float3 baseColor;
    uint baseColorTextureId;

    float3 specularColor;
    float ior;

    float emissiveStrength;
    float3 emissiveColor;

    bool hasDiffuse()
    {
        return bool(flags & MATERIAL_FLAG_HAS_DIFFUSE);
    }

    bool hasSpecularReflection()
    {
        return bool(flags & MATERIAL_FLAG_HAS_SPECULAR);
    }

    bool hasEmission()
    {
        return emissiveStrength > 0.f;
    }

    bool canReflect()
    {
        return bool(flags & MATERIAL_FLAG_HAS_SPECULAR); // TODO: add more conditions here later?
    }

    bool canTransmit() // TODO: use a more appropriate word than "transmit"?
    {
        return bool(flags & MATERIAL_FLAG_HAS_DIFFUSE); // TODO: add more conditions here later? (e.g. specular transmission)
    }

    bool canScatter()
    {
        return canReflect() || canTransmit();
    }

#if _hlsl
    float3 getEmissiveColor()
    {
        return emissiveColor * emissiveStrength;
    }
#else
    void setHasDiffuse(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_HAS_DIFFUSE) | (-uint32_t(enable) & MATERIAL_FLAG_HAS_DIFFUSE);
    }

    void setHasSpecularReflection(bool enable)
    {
        flags = (flags & ~MATERIAL_FLAG_HAS_SPECULAR) | (-uint32_t(enable) & MATERIAL_FLAG_HAS_SPECULAR);
    }
#endif
};

struct AreaLight
{
    float3 pos0_WS;
    uint instanceId;

    float3 pos1_WS;
    uint triangleIdx;

    float3 pos2_WS;
    float rcpArea;

    float3 normal_WS;
    uint pad0;
};

struct CameraParams
{
    float3 pos_WS;
    uint pad0;

    float3 forward_WS;
    uint pad1;

    float3 right_WS;
    uint pad2;

    float3 up_WS;
    float tanHalfFovY;
};

struct SceneParams
{
    uint frameNumber;
    uint numAreaLights;
    uint pad0;
    uint pad1;
};

#if !_hlsl
#undef uint

#undef float2
#undef float3
#endif // !_hlsl
