#pragma once

#include <WindowsNumerics.h>

using namespace Windows::Foundation::Numerics;

using uint = unsigned int;

struct Vertex
{
    float3 pos;
    float3 nor;  // TODO: pack into one? uint
    float2 uv;   // TODO: pack into one uint
};

struct InstanceData
{
    uint vertBufferOffset;
    uint hasIdx;
    uint idxBufferByteOffset;
};
