// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
#include "materials.hlsli"
#include "payload.hlsli"
#include "util/ray.hlsli"

#ifndef NRC_QUERY
    #define NRC_QUERY 0
#endif
#ifndef NRC_UPDATE
    #define NRC_UPDATE 0
#endif

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(RT, RAYTRACING_ACS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT, INSTANCE_DATAS);
StructuredBuffer<PerTriangleData> perTriDatas : REGISTER_T(RT, PER_TRI_DATAS);

StructuredBuffer<Vertex> verts : REGISTER_T(RT, VERTS);
ByteAddressBuffer idxs : REGISTER_T(RT, IDXS);

#include "volume.hlsli"

uint getPathSplitIdx()
{
#if defined(RC_UPDATE) || NRC_UPDATE
    return 0;
#else
    if (bool(renderParams.doPathSplitting))
    {
        return DispatchRaysIndex().x % 2;
    }
    else
    {
        return 0;
    }
#endif
}

uint2 getPixelIdx()
{
    const uint2 dispatchIdx = DispatchRaysIndex().xy;
    if (bool(renderParams.doPathSplitting))
    {
        return uint2(dispatchIdx.x / 2, dispatchIdx.y);
    }
    else
    {
        return dispatchIdx;
    }
}

bool isPixelOutOfBounds(int2 pixelIdx)
{
    return any(pixelIdx < int2(0, 0)) || any(pixelIdx >= renderParams.renderSize);
}

void loadVertsFromInstance(const InstanceData instanceData, const uint triIdx, out Vertex v0, out Vertex v1, out Vertex v2)
{
    uint i0, i1, i2;
    if (bool(instanceData.hasIdxs))
    {
        const uint idxsBufferByteOffset = instanceData.idxsBufferByteOffset + triIdx * 3 * 4;
        i0 = idxs.Load(idxsBufferByteOffset + 0);
        i1 = idxs.Load(idxsBufferByteOffset + 4);
        i2 = idxs.Load(idxsBufferByteOffset + 8);
    }
    else
    {
        i0 = triIdx * 3;
        i1 = i0 + 1;
        i2 = i0 + 2;
    }

    v0 = verts[instanceData.vertsBufferOffset + i0];
    v1 = verts[instanceData.vertsBufferOffset + i1];
    v2 = verts[instanceData.vertsBufferOffset + i2];
}

[shader("anyhit")]
void AnyHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];

    const uint materialIdx = instanceData.materialIdx;
    if (materialIdx == MATERIAL_IDX_INVALID)
    {
        return;
    }

    const Material material = materials[materialIdx];
    const bool testRefractionPassthrough =
        bool(payload.flags & PAYLOAD_FLAG_REFRACTION_PASSTHROUGH) && material.hasGlossyTransmission();
    const bool testAlphaCutout =
        material.hasDiffuse() && material.baseColorTextureId != TEXTURE_ID_INVALID;

    if (!testRefractionPassthrough && !testAlphaCutout)
    {
        return;
    }

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);
    const float2 uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    const float coneWidth = getRayConeWidthAtDistance(payload.rayCone, RayTCurrent());
    const float mipLevel = computeMipLevel(coneWidth);
    const float4 baseColor = getMaterialBaseColor(material, uv, mipLevel);

    if (testRefractionPassthrough)
    {
        payload.pathWeight *= baseColor.rgb;

        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + PrimitiveIndex()];
        if (bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER))
        {
            // Track the first water entry/exit T for absorption in computePassthroughAbsorption.
            // NOTE: tracks only one entry/exit; breaks down for multiple water bodies along the ray.
            if (HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE)
            {
                payload.waterEntryT = min(payload.waterEntryT, RayTCurrent());
            }
            else
            {
                payload.waterExitT = min(payload.waterExitT, RayTCurrent());
            }
        }

        IgnoreHit();
        return;
    }

    if (baseColor.a < 0.999f) // testAlphaCutout
    {
        if (baseColor.a == 0.f)
        {
            IgnoreHit();
            return;
        }

        // If path splitting is enabled, we will split for fractional opacity, so we don't want to ignore those hits in the gbuffer pass.
        const bool checkFractionalOpacity =
            !bool(renderParams.doPathSplitting) || !bool(payload.flags & PAYLOAD_FLAG_IS_GBUFFER);
        if (checkFractionalOpacity && payload.rng.nextFloat() > baseColor.a)
        {
            IgnoreHit();
            return;
        }
    }
}

[shader("closesthit")]
void ClosestHit_Primary(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float3 hitPos_OS = v0.pos_OS * bary.x + v1.pos_OS * bary.y + v2.pos_OS * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), ObjectToWorld4x3()).xyz;

    const float3 hitNor_OS = v0.nor * bary.x + v1.nor * bary.y + v2.nor * bary.z;
    float3 nor_WS = normalize(mul(hitNor_OS, (float3x3) WorldToObject3x4()));
    if (dot(nor_WS, -WorldRayDirection()) < 0.f)
    {
        nor_WS = -nor_WS;
        payload.flags |= PAYLOAD_FLAG_BACKFACE_HIT;
    }
    payload.hitInfo.hitNor_WS = nor_WS;

    payload.hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    payload.materialIdx = instanceData.materialIdx;

    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags &= ~PAYLOAD_FLAG_DID_HIT;
}
