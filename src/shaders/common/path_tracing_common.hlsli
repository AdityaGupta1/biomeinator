// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/payload.hlsli"
#include "common/water_waves.hlsli"
#include "materials/materials.hlsli"
#include "util/ray.hlsli"

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(RT, RAYTRACING_ACS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT, INSTANCE_DATAS);
StructuredBuffer<PerTriangleData> perTriDatas : REGISTER_T(RT, PER_TRI_DATAS);

StructuredBuffer<Vertex> verts : REGISTER_T(RT, VERTS);
ByteAddressBuffer idxs : REGISTER_T(RT, IDXS);

#include "materials/volume.hlsli"

uint getPathSplitIdx()
{
#if NRC_UPDATE
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
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + PrimitiveIndex()];
    TexSampleCtx texCtx;
    texCtx.mipLevel = computeMipLevel(coneWidth);
    texCtx.arraySliceIdx = perTriData.texArraySliceIdx;
    const float4 baseColor = getMaterialBaseColor(material, uv, texCtx);

    if (testRefractionPassthrough)
    {
        payload.pathWeight *= baseColor.rgb;

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

    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + PrimitiveIndex()];
    if (bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER_TOP))
    {
        const float2 posXZ_WS = payload.hitInfo.hitPos_WS.xz + float2(cameraParams.globalInstanceOffset.xz);
        const float2 baseGrad = waveHeightAndGradient(posXZ_WS, renderParams.time).yz;
        const float2 grad = baseGrad + waveNormalPerturbation(posXZ_WS, renderParams.time);
        float3 baseNor_WS = normalize(float3(-baseGrad.x, 1.f, -baseGrad.y));
        float3 waveNor_WS = normalize(float3(-grad.x, 1.f, -grad.y));
        if (bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT))
        {
            baseNor_WS = -baseNor_WS;
            waveNor_WS = -waveNor_WS;
        }

        // At grazing incidence, the noise-perturbed normal can reflect rays into this or a
        // neighboring wave, where they return almost no light (mostly lost to volume
        // absorption), showing as flickering black pixels. Clamp the reflection direction
        // to a margin above the unperturbed surface's horizon — enough to clear neighboring
        // waves — and rebuild the normal as the view/reflection half vector, which also
        // keeps the normal in the viewer's hemisphere.
        const float minReflectedDotBase = 0.05f;
        const float3 view_WS = -WorldRayDirection();
        const float3 reflected_WS = reflect(WorldRayDirection(), waveNor_WS);
        const float reflectedDotBase = dot(reflected_WS, baseNor_WS);
        if (reflectedDotBase < minReflectedDotBase)
        {
            const float3 clampedReflected_WS = normalize(reflected_WS + baseNor_WS * (minReflectedDotBase - reflectedDotBase));
            waveNor_WS = normalize(view_WS + clampedReflected_WS);
        }
        nor_WS = waveNor_WS;
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
