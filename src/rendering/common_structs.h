#pragma once

#include <DirectXMath.h>

using namespace DirectX;

struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT3 nor; // TODO: pack into one? uint
    XMFLOAT2 uv;  // TODO: pack into one uint
};

struct InstanceData
{
    uint32_t vertBufferOffset;
    uint32_t hasIdxs;
    uint32_t idxBufferByteOffset;
};

struct CameraParams
{
    XMFLOAT3 pos_WS;
    uint32_t pad0;

    XMFLOAT3 forward_WS;
    uint32_t pad1;

    XMFLOAT3 right_WS;
    uint32_t pad2;

    XMFLOAT3 up_WS;
    float tanHalfFovY;
};
