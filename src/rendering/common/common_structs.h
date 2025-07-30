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

struct Material
{
#if !_hlsl
public:
    Material();
#endif

    uint hasDiffuse : 1;
    uint pad0 : 31;
    uint pad1;
    uint pad2;
    uint pad3;

    float3 diffuseColor;
    uint diffuseTextureId;

    float emissiveStrength;
    float3 emissiveColor;
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
