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

#define NUM_SAMPLES_PER_PIXEL 512
#define MAX_PATH_DEPTH 12

StructuredBuffer<Vertex> verts : REGISTER_T(REGISTER_VERTS, REGISTER_SPACE_BUFFERS);
ByteAddressBuffer idxs : REGISTER_T(REGISTER_IDXS, REGISTER_SPACE_BUFFERS);

float3 calculateRayTarget(const float2 idx, const float2 size)
{
    const float2 uv = idx / size;
    const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.tanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 target = cameraParams.pos_WS
        + cameraParams.right_WS * ndc.x * xScale
        + cameraParams.up_WS * ndc.y * yScale
        + cameraParams.forward_WS;
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

float powerHeuristic(const float pdfA, const float pdfB)
{
    const float pdfA2 = pdfA * pdfA;
    const float pdfB2 = pdfB * pdfB;
    return pdfA2 / (pdfA2 + pdfB2);
}

#if 0
void pathTraceRay(RayDesc ray, inout Payload payload)
{
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

    if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialId == MATERIAL_ID_INVALID)
    {
        return;
    }

    for (uint pathDepth = 0; pathDepth < MAX_PATH_DEPTH; ++pathDepth)
    {
        const Material material = materials[payload.materialId];

        if (material.hasEmission())
        {
            payload.pathColor += payload.pathWeight * material.getEmissiveColor();
        }

        const bool isLastBounce = pathDepth == MAX_PATH_DEPTH - 1;
        if (!material.canScatter() || isLastBounce)
        {
            return;
        }

        float3 hitPos_WS, normal_WS;
        calcPathPosAndNormal(ray, payload, hitPos_WS, normal_WS);
        const float3 wo_WS = -ray.Direction;

        const BsdfSample sample = sampleBsdf(material, payload.hitInfo.uv, wo_WS, normal_WS, payload.rng);

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

        // russian roulette
        if (pathDepth >= 3)
        {
            const float survivalProbability = max(saturate(luminance(payload.pathWeight)), 0.1f);
            if (payload.rng.nextFloat() >= survivalProbability)
            {
                payload.pathColor = 0;
                return;
            }
            payload.pathWeight /= survivalProbability;
        }

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialId == MATERIAL_ID_INVALID)
        {
            return;
        }
    }
}
#else
void pathTraceRay(RayDesc ray, inout Payload payload)
{
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

    if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialId == MATERIAL_ID_INVALID)
    {
        return;
    }

    for (uint pathDepth = 0; pathDepth < MAX_PATH_DEPTH; ++pathDepth)
    {
        const Material surfMaterial = materials[payload.materialId];

        if (surfMaterial.hasEmission())
        {
            payload.pathColor += payload.pathWeight * surfMaterial.getEmissiveColor();
        }

        const bool isLastBounce = pathDepth == MAX_PATH_DEPTH - 1;
        if (!surfMaterial.canScatter() || isLastBounce) // TODO: check if isLastBounce condition is correct when MAX_PATH_DEPTH == 1
        {
            return;
        }

        float3 surfPos_WS, surfNor_WS;
        calcPathPosAndNormal(ray, payload, surfPos_WS, surfNor_WS);
        const float3 wo_WS = -ray.Direction;

        // TODO: don't do this if material is only specular refl/trans and has 0 roughness
        const DirectLightingSample lightSample = sampleDirectLighting(surfPos_WS, surfNor_WS, payload.rng);
        if (lightSample.didHitLight)
        {
            // TODO: reuse fresnel reflectance from evaluateBsdf() in bsdfPdf()
            const float3 bsdfVal = evaluateBsdf<true /*calculateFresnelReflectance*/>(
                surfMaterial,
                payload.hitInfo.uv,
                wo_WS,
                lightSample.wi_WS,
                surfNor_WS);
            const float bsdfPdf = bsdfPdf(surfMaterial, wo_WS, lightSample.wi_WS, surfNor_WS);
            const float misWeight = powerHeuristic(lightSample.pdf, bsdfPdf);
            payload.pathColor += payload.pathWeight * bsdfVal * absCosTheta(lightSample.wi_WS, surfNor_WS) * misWeight
                * lightSample.Le / lightSample.pdf;
        }

        const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, payload.rng);

        float3 adjustedBsdfValue = surfBsdfSample.bsdfValue;
        if (!surfBsdfSample.wasSpecular)
        {
            adjustedBsdfValue *= absCosTheta(surfBsdfSample.wi_WS, surfNor_WS) / surfBsdfSample.pdf;
        }
        payload.pathWeight *= adjustedBsdfValue;

        // TODO: figure out where to put this and re-enable it
        //// russian roulette
        //if (pathDepth >= 3)
        //{
        //    const float survivalProbability = max(saturate(luminance(payload.pathWeight)), 0.1f);
        //    if (payload.rng.nextFloat() >= survivalProbability)
        //    {
        //        payload.pathColor = 0;
        //        return;
        //    }
        //    payload.pathWeight /= survivalProbability;
        //}

        ray.Origin = surfPos_WS + 0.001f * surfNor_WS;
        ray.Direction = surfBsdfSample.wi_WS;
        ray.TMin = 0.f;
        ray.TMax = 10000.f;

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialId == MATERIAL_ID_INVALID)
        {
            return;
        }

        const Material hitMaterial = materials[payload.materialId];
        if (hitMaterial.hasEmission() && !surfBsdfSample.wasSpecular)
        {
            float3 hitPos_WS, hitNor_WS;
            calcPathPosAndNormal(ray, payload, hitPos_WS, hitNor_WS);

            const float lightPdf = lightPdf(payload.hitInfo, ray.Direction);
            const float misWeight = powerHeuristic(surfBsdfSample.pdf, lightPdf);
            payload.pathWeight *= misWeight;
        }
    }
}
#endif

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

    const Vertex v0 = verts[instanceData.vertBufferOffset + i0];
    const Vertex v1 = verts[instanceData.vertBufferOffset + i1];
    const Vertex v2 = verts[instanceData.vertBufferOffset + i2];

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
