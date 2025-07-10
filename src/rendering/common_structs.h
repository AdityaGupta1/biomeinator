#pragma once

#ifndef __hlsl
#define __hlsl 0
#endif

#if !__hlsl
#include <DirectXMath.h>

#define uint uint32_t

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3
#endif // !__hlsl

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

struct Material
{
#if !__hlsl
public:
    Material();
#endif

    float diffuseWeight;
    float3 diffuseColor;

    uint diffuseTextureId;
    uint pad0;
    uint pad1;
    uint pad2;

    float specularWeight;
    float3 specularColor;

    float emissiveStrength;
    float3 emissiveColor;
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
    uint pad0;
    uint pad1;
    uint pad2;
};

#if !__hlsl
#undef uint

#undef float2
#undef float3
#endif // !__hlsl
