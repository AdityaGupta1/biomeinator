#pragma once

#ifndef __HLSL_VERSION
#define _hlsl 0
#else
#define _hlsl 1
#endif

#if !_hlsl
#include <DirectXMath.h>

#define uint uint32_t

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3
#endif // !_hlsl

// u#
#define REGISTER_RENDER_TARGET 0

// t#
#define REGISTER_RAYTRACING_ACS 0
#define REGISTER_VERTS 1
#define REGISTER_IDXS 2
#define REGISTER_INSTANCE_DATAS 3
#define REGISTER_MATERIALS 4
#define REGISTER_TEXTURES 5

// b#
#define REGISTER_GLOBAL_PARAMS 0

// s#
#define REGISTER_TEX_SAMPLER 0

#if _hlsl
#define _REGISTER_IMPL(type, reg) register(type##reg)
#define REGISTER_U(reg) _REGISTER_IMPL(u, reg)
#define REGISTER_T(reg) _REGISTER_IMPL(t, reg)
#define REGISTER_B(reg) _REGISTER_IMPL(b, reg)
#define REGISTER_S(reg) _REGISTER_IMPL(s, reg)
#endif

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

#if !_hlsl
#undef uint

#undef float2
#undef float3
#endif // !_hlsl
