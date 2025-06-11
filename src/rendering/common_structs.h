#pragma once

#include <DirectXMath.h>

struct Vertex
{
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 nor; // TODO: pack into one? uint
    DirectX::XMFLOAT2 uv;  // TODO: pack into one uint
};

struct InstanceData
{
    uint32_t vertBufferOffset;
    uint32_t hasIdxs;
    uint32_t idxBufferByteOffset;
};

struct CameraParams
{
    DirectX::XMFLOAT3 pos_WS;
    uint32_t pad0;

    DirectX::XMFLOAT3 forward_WS;
    uint32_t pad1;

    DirectX::XMFLOAT3 right_WS;
    uint32_t pad2;

    DirectX::XMFLOAT3 up_WS;
    float tanHalfFovY;
};
