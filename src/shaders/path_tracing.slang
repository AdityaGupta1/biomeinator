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

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "global_params.slang"
#include "light_sampling.slang"
#include "materials.slang"
#include "payload.slang"
#include "util/color.slang"
#include "util/math.slang"

#define NUM_SAMPLES_PER_PIXEL 4096
#define MAX_PATH_DEPTH 12

StructuredBuffer<Vertex> verts : REGISTER_T(REGISTER_VERTS, REGISTER_SPACE_BUFFERS);
ByteAddressBuffer idxs : REGISTER_T(REGISTER_IDXS, REGISTER_SPACE_BUFFERS);

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

    // material has no reflectance
    if (material.hasDiffuse == 0 && material.hasSpecularReflection == 0)
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

    BsdfSample sample = sampleBsdf(material, payload.hitInfo.uv, wo_WS, normal_WS, payload.rng);

    float3 adjustedBsdfValue = sample.bsdfValue / sample.pdf;
    if (!sample.wasSpecular)
    {
        adjustedBsdfValue *= absCosTheta(sample.wi_WS, normal_WS);
    }
    payload.pathWeight *= adjustedBsdfValue;

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

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED))
        {
            return true;
        }

        if (payload.materialId == MATERIAL_ID_INVALID)
        {
            return false;
        }

        const bool isLastBounce = pathDepth == MAX_PATH_DEPTH - 1;
        bounceRay(ray, payload, isLastBounce);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED))
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
    payload.pathWeight *= bsdfValue * absCosTheta(lightSample.wi_WS, normal_WS) / lightSample.pdf;
    payload.pathColor += payload.pathWeight * lightSample.Le;

    return true;
}

[shader("closesthit")]
void ClosestHit_Primary(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];

    uint i0, i1, i2;
    if (bool(instanceData.hasIdxs))
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

    payload.materialId = instanceData.materialId;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
}
