#pragma once

#include <WindowsNumerics.h>

using namespace Windows::Foundation::Numerics;

struct Vertex
{
    float3 pos;
    float3 nor;  // TODO: pack into one? uint
    float2 uv;   // TODO: pack into one uint
};

struct InstanceData
{
    uint64_t vertBufferOffset;
    uint64_t idxBufferOffset;
};
