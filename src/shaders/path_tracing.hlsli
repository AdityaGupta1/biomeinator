#pragma once

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "global_params.hlsli"
#include "rng.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#define MAX_PATH_DEPTH 8

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

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(REGISTER_RAYTRACING_ACS, REGISTER_SPACE_BUFFERS);

StructuredBuffer<Vertex> verts : REGISTER_T(REGISTER_VERTS, REGISTER_SPACE_BUFFERS);
ByteAddressBuffer idxs : REGISTER_T(REGISTER_IDXS, REGISTER_SPACE_BUFFERS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(REGISTER_INSTANCE_DATAS, REGISTER_SPACE_BUFFERS);

StructuredBuffer<Material> materials : REGISTER_T(REGISTER_MATERIALS, REGISTER_SPACE_BUFFERS);

Texture2D<float4> textures[MAX_NUM_TEXTURES] : REGISTER_T(REGISTER_TEXTURES, REGISTER_SPACE_TEXTURES);
SamplerState texSampler : REGISTER_S(REGISTER_TEX_SAMPLER, REGISTER_SPACE_TEXTURES);

StructuredBuffer<AreaLight> areaLights : REGISTER_T(REGISTER_AREA_LIGHTS, REGISTER_SPACE_BUFFERS);
StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE, REGISTER_SPACE_BUFFERS);

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

void evaluateBsdf(inout RayDesc ray, inout Payload payload)
{
    const Material material = materials[payload.materialId];

    // TODO: remove
    if (true)
    {
        bool hitLight = false;
        for (uint lightIdx = 0; lightIdx < sceneParams.numAreaLights; ++lightIdx)
        {
            AreaLight light = areaLights[lightIdx];
            if (payload.hitInfo.instanceId == light.instanceId && payload.hitInfo.triangleIdx == light.triangleIdx)
            {
                hitLight = true;
                break;
            }
        }

        payload.pathColor = hitLight ? float3(0, 1, 0) : float3(1, 0, 0);
        payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
        return;
    }

    if (material.emissiveStrength > 0)
    {
        payload.pathColor += payload.pathWeight * material.emissiveColor * material.emissiveStrength;
    }

    const float3 hitPos_WS = evalRayPos(ray, payload.hitInfo.hitT);
    const float3 normal_WS = faceforward(payload.hitInfo.normal_WS, -ray.Direction);

    const float totalWeight = material.diffuseWeight + material.specularWeight;
    if (totalWeight == 0)
    {
        payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
        return;
    }

    // for now, pick either fully diffuse or fully specular
    // will deal with proper layer selection when I have more complicated bsdfs (rough reflection, etc.)

    if (material.diffuseWeight > 0)
    {
        float3 diffuseColor = material.diffuseColor;
        if (material.diffuseTextureId != TEXTURE_ID_INVALID)
        {
            diffuseColor = textures[material.diffuseTextureId].SampleLevel(texSampler, payload.hitInfo.uv, 0).rgb;
        }
        payload.pathWeight *= diffuseColor;

        const float2 rndSample = float2(payload.rng.nextFloat(), payload.rng.nextFloat());
        const float3 newDir_WS = sampleHemisphereCosineWeighted(normal_WS, rndSample);

        ray.Origin = hitPos_WS;
        ray.Direction = newDir_WS;
        ray.TMin = 0.001;
        ray.TMax = 1000;
    }
    else // material.specularWeight > 0
    {
        payload.pathWeight *= material.specularColor;

        const float3 reflectedDir_WS = reflect(normalize(ray.Direction), normal_WS);

        ray.Origin = hitPos_WS;
        ray.Direction = reflectedDir_WS;
        ray.TMin = 0.001;
        ray.TMax = 1000;
    }
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

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

        if (payload.flags & PAYLOAD_FLAG_PATH_FINISHED)
        {
            return true;
        }

        if (payload.materialId == MATERIAL_ID_INVALID)
        {
            return false;
        }

        evaluateBsdf(ray, payload);

        if (payload.flags & PAYLOAD_FLAG_PATH_FINISHED)
        {
            return true;
        }
    }

    return false;
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
    const float3 Li = float3(0, 0, 0);
    payload.pathColor += payload.pathWeight * Li;
    payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
}
