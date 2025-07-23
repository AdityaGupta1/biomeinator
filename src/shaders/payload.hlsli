#pragma once

#include "rng.hlsli"

#define PAYLOAD_FLAG_PATH_FINISHED (1 << 0)

struct HitInfo
{
    float3 normal_WS;
    float hitT;

    float2 uv;
    uint instanceId;
    uint triangleIdx;
};

struct Payload
{
    float3 pathWeight;
    uint flags;

    float3 pathColor;
    uint materialId;

    HitInfo hitInfo;

    RandomSampler rng;
};
