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
    uint32_t hasIdx;
    uint32_t idxBufferByteOffset;
};

struct CameraParams
{
    XMFLOAT3 right_WS;
    XMFLOAT3 up_WS;
    XMFLOAT3 forward_WS;
    float tanHalfFovY;
};
