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

#define float4x4 DirectX::XMFLOAT4X4
#endif // !_hlsl

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
    float3 pos;
    float3 nor; // TODO: pack into one? uint
    float2 uv; // TODO: pack into one uint
};

struct InstanceData
{
    uint vertsBufferOffset;
    uint hasIdxs;
    uint idxsBufferByteOffset;
    uint perTriDatasBufferOffset;

    uint areaLightsBufferOffset;
    uint materialIdx;
    uint pad1;
    uint pad2;
};

#define MATERIAL_IDX_INVALID ~0u
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

    bool isOnlySpecular()
    {
        return (flags == MATERIAL_FLAG_HAS_SPECULAR);
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

#define LIGHT_ID_INVALID ~0u

struct PerTriangleData
{
#if !_hlsl
public:
    PerTriangleData();
#endif

    uint localAreaLightIdx;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct HeapIndices
{
    struct
    {
        uint pathTracingTargetIdx;
        uint diffuseAlbedoTargetIdx;
        uint specularAlbedoTargetIdx;
        uint linearDepthTargetIdx;

        uint normalsAndRoughnessTargetIdx;
        uint motionTargetIdx;
        uint specularHitDistanceTargetIdx;
        uint debugTargetIdx;
    } uav;

    struct
    {
        uint pathTracingTargetIdx;
        uint diffuseAlbedoTargetIdx;
        uint specularAlbedoTargetIdx;
        uint linearDepthTargetIdx;

        uint normalsAndRoughnessTargetIdx;
        uint motionTargetIdx;
        uint specularHitDistanceTargetIdx;
        uint dlssOutputTargetIdx;

        uint debugTargetIdx;
        uint pad0;
        uint pad1;
        uint pad2;
    } srv;
};

struct ConstantParams
{
    uint rngSeed;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct CameraParams
{
    float4x4 worldToClipMat;
    float4x4 worldToPrevClipMat;

    float2 jitter;
    uint pad0;
    uint pad1;

    float3 pos_WS;
    float nearPlane;

    float3 forward_WS;
    float farPlane;

    float3 right_WS;
    uint pad2;

    float3 up_WS;
    float tanHalfFovY;
};

struct SceneParams
{
    uint numAreaLights;
    uint pad0;
    uint pad1;
    uint pad2;
};

enum class Tonemapping : uint
{
    NONE,
    STANDARD,
    AGX,
    KHRONOS_PBR_NEUTRAL,

    COUNT
};

struct RenderParams
{
    uint frameNumber;
    uint numSamplesPerPixel;
    uint maxPathDepth;
    uint enableMis;

    uint tonemapping;
    uint preTonemappedColorSrvIdx;
    uint pad1;
    uint pad2;
};

struct DebugParams
{
    uint debugOutputSrvIdx;
    uint debugOutputNumChannels;
    float debugOutputScale;
    uint pad0;

    uint debugBool0;
    uint debugBool1;
    uint debugBool2;
    uint debugBool3;

    float debugFloat0;
    float debugFloat1;
    float debugFloat2;
    float debugFloat3;
};

#if !_hlsl
#undef uint

#undef float2
#undef float3

#undef float4x4
#endif // !_hlsl
