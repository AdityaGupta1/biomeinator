#pragma once

struct Vertex
{
    float3 pos : POSITION;
    float3 nor : POSITION;
    float2 uv : TEXCOORD0;
};

struct InstanceData
{
    uint vertBufferOffset;
    uint hasIdx;
    uint idxBufferByteOffset;
};

struct CameraParams
{
    float3 pos_WS;
    uint pad0;
    
    float3 right_WS;
    uint pad1;
    
    float3 up_WS;
    uint pad2;
    
    float3 forward_WS;
    float tanHalfFovY;
};
