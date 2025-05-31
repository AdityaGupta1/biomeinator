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
