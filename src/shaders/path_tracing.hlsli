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

#include "global_params.hlsli"
#include "light_sampling.hlsli"
#include "materials.hlsli"
#include "payload.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

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
    return mad(ray.Direction, t, ray.Origin);
}

float powerHeuristic(const float pdfA, const float pdfB)
{
    const float pdfA2 = pdfA * pdfA;
    const float pdfB2 = pdfB * pdfB;
    return pdfA2 / (pdfA2 + pdfB2);
}

float2 calculateMotionFromPos(const float3 pos_WS)
{
    float4 currNdc = mul(cameraParams.worldToClipMat, float4(pos_WS, 1));
    currNdc /= currNdc.w;
    float4 prevNdc = mul(cameraParams.worldToPrevClipMat, float4(pos_WS, 1));
    prevNdc /= prevNdc.w;

    float2 motion = (prevNdc.xy - currNdc.xy) / 2;
    motion.y = -motion.y;
    //motion *= DispatchRaysDimensions().xy;
    return motion;
}

void outputGuideBuffers(const Payload payload, const RayDesc ray)
{
    const uint2 pixelIdx = payload.pixelIdx;

    float3 diffuseAlbedo = 0.f;
    float linearDepth = cameraParams.farPlane;
    float3 motionHitPos_WS;
    float3 hitNor_WS = 0.f;
    float roughness = 0.f;
    float3 specularAlbedo = 0.f;

    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        const Material surfMaterial = materials[payload.materialId];
        diffuseAlbedo = getMaterialDiffuseAlbedo(surfMaterial, payload.hitInfo.uv);

        linearDepth = distance(ray.Origin, payload.hitInfo.hitPos_WS);

        motionHitPos_WS = payload.hitInfo.hitPos_WS;
        hitNor_WS = payload.hitInfo.hitNor_WS;

        // TODO: eventually set roughness

        if (surfMaterial.hasSpecularReflection())
        {
            const float alpha = roughness * roughness;
            const float nDotV = dot(hitNor_WS, -ray.Direction);
            specularAlbedo = calculateDlssSpecularAlbedo(surfMaterial.specularColor, alpha, nDotV);
        }
    }
    else
    {
        motionHitPos_WS = evalRayPos(ray, cameraParams.farPlane);
        hitNor_WS = normalize(-ray.Direction);

        specularAlbedo = 0.5f; // this was suggested somewhere for miss specular albedo (I forgot where though)
    }

    RWTexture2D<float4> diffuseAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.diffuseAlbedoTargetIdx];
    diffuseAlbedoTarget[pixelIdx] = float4(diffuseAlbedo, 1);

    RWTexture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.uav.linearDepthTargetIdx];
    linearDepthTarget[pixelIdx] = linearDepth;

    RWTexture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.uav.motionTargetIdx];
    motionTarget[pixelIdx] = calculateMotionFromPos(motionHitPos_WS);

    RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
    normalsAndRoughnessTarget[pixelIdx].xyzw = float4(hitNor_WS, roughness);

    RWTexture2D<float4> specularAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.specularAlbedoTargetIdx];
    specularAlbedoTarget[pixelIdx] = float4(specularAlbedo, 1);
}

void pathTraceRay(RayDesc ray, inout Payload payload, bool isFirstSample)
{
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

    if (isFirstSample)
    {
        outputGuideBuffers(payload, ray);
    }

    if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialId == MATERIAL_ID_INVALID)
    {
        return;
    }

    for (uint pathDepth = 0; pathDepth < renderParams.maxPathDepth; ++pathDepth)
    {
        const Material surfMaterial = materials[payload.materialId];

        if (surfMaterial.hasEmission())
        {
            payload.pathColor += payload.pathWeight * surfMaterial.getEmissiveColor();
        }

        const bool isLastBounce = pathDepth == renderParams.maxPathDepth - 1;
        if (!surfMaterial.canScatter() || isLastBounce)
        {
            return;
        }

        // russian roulette
        if (pathDepth >= 2)
        {
             const float survivalProbability = max(saturate(luminance(payload.pathWeight)), 0.1f);
             if (payload.rng.nextFloat() >= survivalProbability)
             {
                 return;
             }
             payload.pathWeight /= survivalProbability;
        }

        const float3 wo_WS = -ray.Direction;
        const float3 surfPos_WS = payload.hitInfo.hitPos_WS;
        const float3 surfNor_WS = faceforward(payload.hitInfo.hitNor_WS, wo_WS);

        if (renderParams.enableMis == 1)
        {
            if (!surfMaterial.isOnlySpecular())
            {
                const DirectLightingSample lightSample = sampleDirectLighting(surfPos_WS, surfNor_WS, payload.rng);
                if (lightSample.didHitLight)
                {
                    // TODO: reuse fresnel reflectance from evaluateBsdf() in bsdfPdf()
                    const float3 bsdfVal = evaluateBsdf(
                        surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS, true /*calculateFresnelReflectance*/);
                    const float lightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, lightSample.wi_WS, surfNor_WS);
                    const float misWeight = powerHeuristic(lightSample.pdf, lightSampleBsdfPdf);
                    payload.pathColor += payload.pathWeight * bsdfVal * absCosTheta(lightSample.wi_WS, surfNor_WS) * misWeight
                        * lightSample.Le / lightSample.pdf;
                }
            }
        }

        const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, payload.rng);

        float3 adjustedBsdfValue = surfBsdfSample.bsdfValue / surfBsdfSample.pdf;
        if (!surfBsdfSample.wasSpecular)
        {
            adjustedBsdfValue *= absCosTheta(surfBsdfSample.wi_WS, surfNor_WS);
        }
        payload.pathWeight *= adjustedBsdfValue;

        ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
        ray.Direction = surfBsdfSample.wi_WS;
        ray.TMin = 0.f;
        ray.TMax = 10000.f;

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialId == MATERIAL_ID_INVALID)
        {
            return;
        }

        if (isFirstSample && pathDepth == 0 && surfBsdfSample.wasSpecular)
        {
            payload.specularHitDistance = distance(surfPos_WS, payload.hitInfo.hitPos_WS);
        }

        if (renderParams.enableMis == 1)
        {
            const Material hitMaterial = materials[payload.materialId];
            if (hitMaterial.hasEmission() && !surfBsdfSample.wasSpecular)
            {
                const float bsdfSampleLightPdf = lightPdf(payload.hitInfo, surfPos_WS, ray.Direction);
                const float misWeight = powerHeuristic(surfBsdfSample.pdf, bsdfSampleLightPdf);
                payload.pathWeight *= misWeight;
            }
            // if BSDF sampling didn't hit a light, lightPdf = 0 (I think) so misWeight = 1
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

    const float4x3 objectToWorldMat = ObjectToWorld4x3();

    const float3 hitPos_OS = v0.pos * bary.x + v1.pos * bary.y + v2.pos * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), objectToWorldMat).xyz;

    const float3 hitNor_OS = v0.nor * bary.x + v1.nor * bary.y + v2.nor * bary.z;
    payload.hitInfo.hitNor_WS = normalize(mul(hitNor_OS, (float3x3)objectToWorldMat));

    payload.hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    payload.materialId = instanceData.materialId;

    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags |= PAYLOAD_FLAG_PATH_FINISHED;
}
