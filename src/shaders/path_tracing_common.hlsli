/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
#include "materials.hlsli"
#include "payload.hlsli"

#define RAY_DEFAULT_TMAX 10000.f
#define RAY_ORIGIN_OFFSET_EPSILON 0.0001f

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(RT, RAYTRACING_ACS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT, INSTANCE_DATAS);
StructuredBuffer<PerTriangleData> perTriDatas : REGISTER_T(RT, PER_TRI_DATAS);

StructuredBuffer<Vertex> verts : REGISTER_T(RT, VERTS);
ByteAddressBuffer idxs : REGISTER_T(RT, IDXS);

static const float3 waterSigmaA = float3(0.35f, 0.08f, 0.02f) * 0.5f;

bool isWaterTriangle(const uint instanceId, const uint triangleIdx)
{
    if (sceneParams.voxelMode == 0)
    {
        return false;
    }

    const InstanceData instanceData = instanceDatas[instanceId];
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + triangleIdx];
    return bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
}

bool isWaterHit(const Payload payload)
{
    return isWaterTriangle(payload.hitInfo.instanceId, payload.hitInfo.triangleIdx);
}

void setUnderwaterFromFrontOrBackHit(inout Payload payload, const bool wasBackfaceHit)
{
    if (wasBackfaceHit)
    {
        payload.flags &= ~PAYLOAD_FLAG_UNDERWATER;
    }
    else
    {
        payload.flags |= PAYLOAD_FLAG_UNDERWATER;
    }
}

float3 getWaterAbsorptionFactor(const Payload payload, const float distance)
{
    if (sceneParams.voxelMode == 0 ||
        !bool(payload.flags & PAYLOAD_FLAG_UNDERWATER) ||
        distance <= 0.f)
    {
        return float3(1.f, 1.f, 1.f);
    }

    return exp(-waterSigmaA * distance);
}

void applyWaterAbsorption(inout Payload payload, const float distance)
{
    payload.pathWeight *= getWaterAbsorptionFactor(payload, distance);
}

float getDistanceToVoxelBounds(const RayDesc ray)
{
    const float3 boundsMin = float3(sceneParams.voxelBoundsMin_WS);
    const float3 boundsMax = float3(sceneParams.voxelBoundsMax_WS);

    float tEnter = 0.f;
    float tExit = 1e30f;

    [unroll]
    for (uint axis = 0; axis < 3; ++axis)
    {
        const float origin = ray.Origin[axis];
        const float dir = ray.Direction[axis];
        const float bMin = boundsMin[axis];
        const float bMax = boundsMax[axis];

        if (abs(dir) < 1e-8f)
        {
            if (origin < bMin || origin > bMax)
            {
                return 0.f;
            }
            continue;
        }

        const float invDir = 1.f / dir;
        float t0 = (bMin - origin) * invDir;
        float t1 = (bMax - origin) * invDir;
        if (t0 > t1)
        {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }

        tEnter = max(tEnter, t0);
        tExit = min(tExit, t1);
    }

    const float nearT = max(tEnter, 0.f);
    if (tExit <= nearT)
    {
        return 0.f;
    }

    return tExit;
}

float3 getSegmentAbsorptionFactor(const Payload payload, const RayDesc ray)
{
    float segmentDistance = 0.f;
    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        segmentDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
    }
    else
    {
        segmentDistance = getDistanceToVoxelBounds(ray);
    }

    return getWaterAbsorptionFactor(payload, segmentDistance);
}

void applySegmentAbsorption(inout Payload payload, const RayDesc ray)
{
    payload.pathWeight *= getSegmentAbsorptionFactor(payload, ray);
}

float getPayloadPassthroughRayT(const Payload payload)
{
    return asfloat(payload.pad0);
}

void setPayloadPassthroughRayT(inout Payload payload, const float rayT)
{
    payload.pad0 = asuint(rayT);
}

void applyPassthroughSegmentAbsorption(inout Payload payload, const float currentRayT)
{
    const float prevRayT = getPayloadPassthroughRayT(payload);
    const float segmentLength = max(currentRayT - prevRayT, 0.f);
    applyWaterAbsorption(payload, segmentLength);
}

void finalizePassthroughRayAbsorption(inout Payload payload, const RayDesc ray)
{
    float rayEndT;
    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        rayEndT = distance(ray.Origin, payload.hitInfo.hitPos_WS);
    }
    else if (ray.TMax < RAY_DEFAULT_TMAX)
    {
        rayEndT = ray.TMax;
    }
    else
    {
        rayEndT = getDistanceToVoxelBounds(ray);
    }

    applyPassthroughSegmentAbsorption(payload, rayEndT);
}

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

float3 getPrimaryRayDirection(const uint2 pixelIdx)
{
    const float2 size = float2(renderParams.renderSize);

    const float2 uv = (pixelIdx + cameraParams.jitter) / size;
    const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.tanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 targetPos_WS = cameraParams.pos_WS
        + cameraParams.right_WS * ndc.x * xScale
        + cameraParams.up_WS * ndc.y * yScale
        + cameraParams.forward_WS;
    return normalize(targetPos_WS - cameraParams.pos_WS);
}

float3 getPrevPrimaryRayDirection(const uint2 pixelIdx)
{
    const float2 size = float2(renderParams.renderSize);

    const float2 uv = (pixelIdx + cameraParams.prevJitter) / size;
    const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.prevTanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 targetPos_WS = cameraParams.prevPos_WS
        + cameraParams.prevRight_WS * ndc.x * xScale
        + cameraParams.prevUp_WS * ndc.y * yScale
        + cameraParams.prevForward_WS;
    return normalize(targetPos_WS - cameraParams.prevPos_WS);
}

void setRayOriginAndDirection(inout RayDesc ray, const float3 origin_WS, float3 normal_WS, const float3 wi_WS, bool faceforwardNormal)
{
    if (faceforwardNormal)
    {
        normal_WS = faceforward(normal_WS, wi_WS);
    }

    ray.Origin = mad(normal_WS, RAY_ORIGIN_OFFSET_EPSILON, origin_WS);
    ray.Direction = wi_WS;
}

float3 evalRayPos(const RayDesc ray, const float t)
{
    return mad(ray.Direction, t, ray.Origin);
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
    const bool maybeRefractionPassthrough =
        bool(payload.flags & PAYLOAD_FLAG_REFRACTION_PASSTHROUGH) && material.hasGlossyTransmission();
    const bool maybeAlphaCutout =
        material.hasDiffuse() && material.baseColorTextureId != TEXTURE_ID_INVALID;

    if (!maybeRefractionPassthrough && !maybeAlphaCutout)
    {
        return;
    }

    if (maybeRefractionPassthrough)
    {
        const float currentRayT = RayTCurrent();
        applyPassthroughSegmentAbsorption(payload, currentRayT);
        setPayloadPassthroughRayT(payload, currentRayT);

        if (isWaterTriangle(InstanceID(), PrimitiveIndex()))
        {
            setUnderwaterFromFrontOrBackHit(payload, HitKind() == HIT_KIND_TRIANGLE_BACK_FACE);
            IgnoreHit();
            return;
        }

        Vertex v0, v1, v2;
        loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

        const float2 bary2 = attribs.barycentrics;
        const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);
        const float2 uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
        const float4 baseColor = getMaterialBaseColor(material, uv);
        payload.pathWeight *= baseColor.rgb;
        IgnoreHit();
        return;
    }

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);
    const float2 uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    const float4 baseColor = getMaterialBaseColor(material, uv);

    if (maybeAlphaCutout && baseColor.a < 0.999f)
    {
        IgnoreHit();
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
