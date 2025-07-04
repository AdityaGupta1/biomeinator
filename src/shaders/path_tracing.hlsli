#pragma once

#include "global_params.hlsli"
#include "rng.hlsli"

#define PAYLOAD_FLAG_ALLOW_REFLECTION (1 << 0)

struct Payload
{
    float3 color;
    uint flags;

    RandomSampler rng;
};

struct HitInfo
{
    float3 normal_OS;
    float2 uv;
};

RaytracingAccelerationStructure scene : register(t0);

StructuredBuffer<Vertex> verts : register(t1);
ByteAddressBuffer idxs : register(t2);

StructuredBuffer<InstanceData> instanceDatas : register(t3);
StructuredBuffer<Material> materials : register(t4);

float3 calculateRayTarget(const float2 idx, const float2 size)
{
    const float2 uv = idx / size;
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.tanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 target = cameraParams.pos_WS
        + cameraParams.right_WS * ndc.x * xScale
        + cameraParams.up_WS * ndc.y * yScale
        + cameraParams.forward_WS * 1.0;
    return target;
}

bool pathTraceRay(const RayDesc ray, inout Payload payload)
{
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    return true;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    float3 skyColor = WorldRayDirection().y > 0 ? float3(1, 1, 1) : 0;
    payload.color = skyColor;
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];

    uint i0, i1, i2;
    if (instanceData.hasIdxs)
    {
        const uint idxBufferByteOffset = instanceData.idxBufferByteOffset + PrimitiveIndex() * 3 * 4;
        i0 = idxs.Load(idxBufferByteOffset + 0);
        i1 = idxs.Load(idxBufferByteOffset + 4);
        i2 = idxs.Load(idxBufferByteOffset + 8);
    }
    else
    {
        i0 = PrimitiveIndex() * 3;
        i1 = i0 + 1;
        i2 = i0 + 2;
    }

    Vertex v0 = verts[instanceData.vertBufferOffset + i0];
    Vertex v1 = verts[instanceData.vertBufferOffset + i1];
    Vertex v2 = verts[instanceData.vertBufferOffset + i2];

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    HitInfo hitInfo;
    hitInfo.normal_OS = v0.nor * bary.x + v1.nor * bary.y + v2.nor * bary.z;
    hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;

    if (instanceData.materialId == MATERIAL_ID_INVALID)
    {
        payload.color = 0;
        return;
    }

    const Material material = materials[instanceData.materialId];

    const float diffuseChance = material.diffWeight / (material.diffWeight + material.specWeight);
    if (payload.rng.nextFloat() < diffuseChance)
    {
        payload.color *= material.diffCol / diffuseChance;
    }
    else
    {
        if (!(payload.flags & PAYLOAD_FLAG_ALLOW_REFLECTION))
        {
            payload.color = 0;
            return;
        }

        payload.color *= material.specCol / (1 - diffuseChance);

        float3 hitPos_WS = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        float3 normal_WS = normalize(mul(hitInfo.normal_OS, (float3x3) ObjectToWorld4x3()));
        float3 reflectedDir_WS = reflect(normalize(WorldRayDirection()), normal_WS);

        RayDesc mirrorRay;
        mirrorRay.Origin = hitPos_WS;
        mirrorRay.Direction = reflectedDir_WS;
        mirrorRay.TMin = 0.001;
        mirrorRay.TMax = 1000;

        payload.flags &= ~PAYLOAD_FLAG_ALLOW_REFLECTION;

        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, mirrorRay, payload);
    }
}
