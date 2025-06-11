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

#if !__hlsl
#undef uint

#undef float2
#undef float3
#endif // !__hlsl
