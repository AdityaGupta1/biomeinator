// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/biome_map.hlsli"
#include "common/global_params.hlsli"
#include "common/payload.hlsli"
#include "common/water_waves.hlsli"
#include "materials/materials.hlsli"
#include "util/packing.hlsli"
#include "util/ray.hlsli"
#include "util/shading_normal.hlsli"

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(RT, RAYTRACING_ACS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT, INSTANCE_DATAS);
StructuredBuffer<PerTriangleData> perTriDatas : REGISTER_T(RT, PER_TRI_DATAS);

StructuredBuffer<Vertex> verts : REGISTER_T(RT, VERTS);
ByteAddressBuffer idxs : REGISTER_T(RT, IDXS);

#include "materials/water.hlsli"

uint getPathSplitIdx()
{
    if (bool(renderParams.doPathSplitting))
    {
        return DispatchRaysIndex().x % 2;
    }
    else
    {
        return 0;
    }
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

// Ctx for surface shading at a hit; samples the biome map once here so all base color reads
// for the hit share the tint (c.f. makeUntintedTexSampleCtx())
TexSampleCtx makeTintedTexSampleCtx(const PerTriangleData perTriData, const float rayConeWidth, const float2 posXZ_WS)
{
    TexSampleCtx texCtx;
    texCtx.mipLevel = computeMipLevel(rayConeWidth);
    texCtx.arraySliceIdx = perTriData.texArraySliceIdx;
    texCtx.biomeTint = getBiomeTint(perTriData.flags, posXZ_WS);
    return texCtx;
}

float4 getMaterialBaseColorAtHit(const Material material, const InstanceData instanceData,
    const PerTriangleData perTriData, const uint triIdx, const float2 bary2, const float mipLevel)
{
    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, triIdx, v0, v1, v2);

    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);
    const float2 uv = unpackUintToFloat2(v0.packedUv) * bary.x + unpackUintToFloat2(v1.packedUv) * bary.y +
                      unpackUintToFloat2(v2.packedUv) * bary.z;

    // Cutout alpha and passthrough absorption don't care about biome tint or the packed aux
    // adjustments, so skip the map sample and the aux texture sample
    const TexSampleCtx texCtx = makeUntintedTexSampleCtx(mipLevel, perTriData.texArraySliceIdx);
    return getMaterialBaseColorNoAux(material, uv, texCtx);
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
    // Only specular transmission can be passed through without scattering; rough glass is a real bounce
    const bool testRefractionPassthrough =
        bool(payload.flags & PAYLOAD_FLAG_REFRACTION_PASSTHROUGH) && material.isDeltaTransmission();
    const bool testAlphaCutout =
        material.hasDiffuse() && material.baseColorTextureId != TEXTURE_ID_INVALID;

    if (!testRefractionPassthrough && !testAlphaCutout)
    {
        return;
    }

    const float coneWidth = getRayConeWidthAtDistance(payload.rayCone, RayTCurrent());
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + PrimitiveIndex()];
    const float4 baseColor = getMaterialBaseColorAtHit(
        material, instanceData, perTriData, PrimitiveIndex(), attribs.barycentrics, computeMipLevel(coneWidth));

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
        if (checkFractionalOpacity && nextFloat(payload.rng) > baseColor.a)
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
    const uint materialIdx = instanceData.materialIdx;

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float3 hitPos_OS = v0.pos_OS * bary.x + v1.pos_OS * bary.y + v2.pos_OS * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), ObjectToWorld4x3()).xyz;

    const float3 hitNor_OS = octDecode(v0.packedNor) * bary.x + octDecode(v1.packedNor) * bary.y + octDecode(v2.packedNor) * bary.z;
    float3 nor_WS = normalize(mul(hitNor_OS, (float3x3) WorldToObject3x4()));
    // Geometric normal, oriented to agree with the interpolated normal so no winding convention is assumed
    float3 geoNor_WS = normalize(mul(cross(v1.pos_OS - v0.pos_OS, v2.pos_OS - v0.pos_OS), (float3x3) WorldToObject3x4()));
    if (dot(geoNor_WS, nor_WS) < 0.f)
    {
        geoNor_WS = -geoNor_WS;
    }

    // Which side of the surface the ray is on is decided by the geometric normal: the interpolated normal can
    // face away from the ray on grazing hits of coarse meshes, and treating those as backfaces would invert the
    // IOR for them
    const float3 wo_WS = -WorldRayDirection();
    if (dot(geoNor_WS, wo_WS) < 0.f)
    {
        geoNor_WS = -geoNor_WS;
        nor_WS = -nor_WS;
        payload.flags |= PAYLOAD_FLAG_BACKFACE_HIT;
    }

    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + PrimitiveIndex()];
    if (bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER_TOP))
    {
        const float2 posXZ_WS = payload.hitInfo.hitPos_WS.xz + float2(cameraParams.globalInstanceOffset.xz);
        nor_WS = waveShadingNormal(posXZ_WS, renderParams.animTime, WorldRayDirection(),
                                   bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT));
    }
    else
    {
        // Glossy lobes need a shading normal whose reflections stay above the surface (as Cycles' bump map
        // correction ensures); other materials keep the plain interpolated normal, facing the ray
        const bool hasGlossy = materialIdx != MATERIAL_IDX_INVALID && materials[materialIdx].hasGlossy();
        if (hasGlossy)
        {
            nor_WS = ensureValidSpecularReflection(geoNor_WS, wo_WS, nor_WS);
        }
        else if (dot(nor_WS, wo_WS) < 0.f)
        {
            nor_WS = -nor_WS;
        }
    }
    payload.hitInfo.hitNor_WS = nor_WS;

    payload.hitInfo.uv = unpackUintToFloat2(v0.packedUv) * bary.x + unpackUintToFloat2(v1.packedUv) * bary.y +
                         unpackUintToFloat2(v2.packedUv) * bary.z;
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    payload.materialIdx = materialIdx;

    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags &= ~PAYLOAD_FLAG_DID_HIT;
}
