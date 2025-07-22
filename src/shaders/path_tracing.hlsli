#pragma once

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "global_params.hlsli"
#include "light_sampling.hlsli"
#include "payload.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#define MAX_PATH_DEPTH 5

StructuredBuffer<Vertex> verts : REGISTER_T(REGISTER_VERTS, REGISTER_SPACE_BUFFERS);
ByteAddressBuffer idxs : REGISTER_T(REGISTER_IDXS, REGISTER_SPACE_BUFFERS);

Texture2D<float4> textures[MAX_NUM_TEXTURES] : REGISTER_T(REGISTER_TEXTURES, REGISTER_SPACE_TEXTURES);
SamplerState texSampler : REGISTER_S(REGISTER_TEX_SAMPLER, REGISTER_SPACE_TEXTURES);

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

float3 evalRayPos(const RayDesc ray, const float t)
{
    return ray.Origin + ray.Direction * t;
}

float3 evaluateBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 normal_WS)
{
    float3 diffuseColor;
    if (material.diffuseTextureId != TEXTURE_ID_INVALID)
    {
        diffuseColor = textures[material.diffuseTextureId].SampleLevel(texSampler, uv, 0).rgb;
    }
    else
    {
        diffuseColor = material.diffuseColor;
    }

    return diffuseColor * M_INV_PI;
}

struct BsdfSample
{
    float3 wi_WS;
    float pdf;

    float3 bsdfValue;
};

BsdfSample sampleBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 normal_WS,
    float2 rndSample)
{
    BsdfSample result;

    const float3 wi_WS = sampleHemisphereCosineWeighted(normal_WS, rndSample);
    result.wi_WS = wi_WS;

    float3 bsdfValue = evaluateBsdf(material, uv, wo_WS, wi_WS, normal_WS);

    result.pdf = cosTheta(wi_WS, normal_WS) / M_PI;
    result.bsdfValue = bsdfValue;

    return result;
}

void calcPathPosAndNormal(const RayDesc ray, const Payload payload, out float3 hitPos_WS, out float3 normal_WS)
{
    hitPos_WS = evalRayPos(ray, payload.hitInfo.hitT);
    normal_WS = faceforward(payload.hitInfo.normal_WS, -ray.Direction);
}

void bounceRay(inout RayDesc ray, inout Payload payload, bool isLastBounce)
{
    const Material material = materials[payload.materialId];

    if (material.emissiveStrength > 0)
    {
        payload.pathColor += payload.pathWeight * material.emissiveColor * material.emissiveStrength;
    }

    if (material.diffuseWeight == 0)
    {
        payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
        return;
    }

    if (isLastBounce)
    {
        return;
    }

    float3 hitPos_WS, normal_WS;
    calcPathPosAndNormal(ray, payload, hitPos_WS, normal_WS);
    float3 wo_WS = -ray.Direction;

    BsdfSample sample = sampleBsdf(material, payload.hitInfo.uv, wo_WS, normal_WS, payload.rng.nextFloat2());

    payload.pathWeight *= sample.bsdfValue * cosTheta(sample.wi_WS, normal_WS) / sample.pdf;

    ray.Origin = hitPos_WS + 0.001f * normal_WS;
    ray.Direction = sample.wi_WS;
    ray.TMin = 0.f;
    ray.TMax = 10000.f;
}

bool pathTraceRay(RayDesc ray, inout Payload payload)
{
    for (uint pathDepth = 0; pathDepth < MAX_PATH_DEPTH; ++pathDepth)
    {
        // russian roulette
        if (pathDepth >= 3)
        {
            const float survivalProbability = max(saturate(luminance(payload.pathWeight)), 0.1f);
            if (payload.rng.nextFloat() >= survivalProbability)
            {
                return false;
            }
            payload.pathWeight /= survivalProbability;
        }

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (payload.flags & PAYLOAD_FLAG_PATH_FINISHED)
        {
            return true;
        }

        if (payload.materialId == MATERIAL_ID_INVALID)
        {
            return false;
        }

        const bool isLastBounce = pathDepth == MAX_PATH_DEPTH - 1;
        bounceRay(ray, payload, isLastBounce);

        if (payload.flags & PAYLOAD_FLAG_PATH_FINISHED)
        {
            return true;
        }
    }

    float3 hitPos_WS, normal_WS;
    calcPathPosAndNormal(ray, payload, hitPos_WS, normal_WS);
    const DirectLightingSample lightSample = sampleDirectLighting(hitPos_WS, normal_WS, payload.rng.nextFloat3());

    if (!lightSample.didHitLight)
    {
        return false;
    }

    float3 bsdfValue = evaluateBsdf(materials[payload.materialId], payload.hitInfo.uv, -ray.Direction, lightSample.wi_WS, normal_WS);
    payload.pathWeight *= bsdfValue * cosTheta(lightSample.wi_WS, normal_WS) / lightSample.pdf;
    payload.pathColor += payload.pathWeight * lightSample.Le;

    return true;
}

[shader("closesthit")]
void ClosestHit_Primary(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
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

    const float3 normal_OS = v0.nor * bary.x + v1.nor * bary.y + v2.nor * bary.z;
    payload.hitInfo.normal_WS = normalize(mul(normal_OS, (float3x3) ObjectToWorld4x3()));
    payload.hitInfo.hitT = RayTCurrent();
    payload.hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    payload.materialId = instanceData.materialId;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
}
