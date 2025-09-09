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

#include "global_params.hlsli"
#include "materials.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"
#include "util/math.hlsli"

StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(RT_REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE, RT_REGISTER_SPACE_BUFFERS);

AreaLight pickLightUniform(inout RandomSampler rng, out float pdf)
{
    const uint lightIdx = areaLightSamplingStructure[uint(rng.nextFloat() * sceneParams.numAreaLights)];
    pdf = 1.f / sceneParams.numAreaLights;
    StructuredBuffer<AreaLight> areaLights = ResourceDescriptorHeap[heapIndices.srv.areaLightsIdx];
    return areaLights[lightIdx];
}

float3 samplePointOnLight(const AreaLight light, inout RandomSampler rng, out float pdf)
{
    const float2 rndSample = rng.nextFloat2();
    const float sqrtRndX = sqrt(rndSample.x);
    const float2 bary2 = float2(1.f - sqrtRndX, sqrtRndX * rndSample.y);
    const float3 pointOnLight_WS = bary2.x * light.pos0_WS + bary2.y * light.pos1_WS + (1.f - bary2.x - bary2.y) * light.pos2_WS;
    pdf = light.rcpArea;
    return pointOnLight_WS;
}

struct DirectLightingSample
{
    bool didHitLight;
    float3 wi_WS;

    float3 Le;
    float pdf;
};

DirectLightingSample sampleDirectLighting(const float3 surfPos_WS, const float3 surfNor_WS, inout RandomSampler rng)
{
    DirectLightingSample result;
    result.didHitLight = false;

    float lightPickPdf;
    const AreaLight light = pickLightUniform(rng, lightPickPdf);

    float lightSamplePdf;
    const float3 pointOnLight_WS = samplePointOnLight(light, rng, lightSamplePdf);

    result.wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    const float r2 = distance2(surfPos_WS, pointOnLight_WS);
    lightSamplePdf *= r2 / absCosTheta(-result.wi_WS, light.normal_WS);

    RayDesc ray;
    ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
    ray.Direction = result.wi_WS;
    ray.TMin = 0.f;
    ray.TMax = 10000.f;

    Payload lightPayload;
    lightPayload.materialId = MATERIAL_ID_INVALID;
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_LIGHTS, 0, 0, ray, lightPayload);

    if (lightPayload.materialId == MATERIAL_ID_INVALID || lightPayload.hitInfo.instanceId != light.instanceId || lightPayload.hitInfo.triangleIdx != light.triangleIdx)
    {
        return result;
    }

    result.didHitLight = true;
    const Material material = materials[lightPayload.materialId];
    result.Le = material.getEmissiveColor();
    result.pdf = lightPickPdf * lightSamplePdf;

    return result;
}

float lightPdf(const HitInfo hitInfo, const float3 surfPos_WS, const float3 wi_WS)
{
    StructuredBuffer<InstanceData> instanceDatas = ResourceDescriptorHeap[heapIndices.srv.instanceDatasIdx];
    const InstanceData instanceData = instanceDatas[hitInfo.instanceId];

    StructuredBuffer<PerTriangleData> perTriDatas = ResourceDescriptorHeap[heapIndices.srv.perTriDatasIdx];
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + hitInfo.triangleIdx];
    if (perTriData.localAreaLightIdx == LIGHT_ID_INVALID)
    {
        return 0.f;
    }

    const uint areaLightIdx = instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx;
    StructuredBuffer<AreaLight> areaLights = ResourceDescriptorHeap[heapIndices.srv.areaLightsIdx];
    const AreaLight light = areaLights[areaLightIdx];
    const float lightPickPdf = 1.f / sceneParams.numAreaLights;
    const float r2 = distance2(surfPos_WS, hitInfo.hitPos_WS);
    return lightPickPdf * light.rcpArea * r2 / absCosTheta(-wi_WS, light.normal_WS);
}

[shader("closesthit")]
void ClosestHit_Lights(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    StructuredBuffer<InstanceData> instanceDatas = ResourceDescriptorHeap[heapIndices.srv.instanceDatasIdx];
    const InstanceData instanceData = instanceDatas[InstanceID()];
    payload.materialId = instanceData.materialId;
}
