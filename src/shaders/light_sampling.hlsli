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

StructuredBuffer<PerTriangleData> perTriDatas : REGISTER_T(PT_REGISTER_PER_TRI_DATAS, PT_REGISTER_SPACE);

StructuredBuffer<AreaLight> areaLights : REGISTER_T(PT_REGISTER_AREA_LIGHTS, PT_REGISTER_SPACE);
StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(PT_REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE, PT_REGISTER_SPACE);

AreaLight sampleLightUniform(const float3 surfPos_WS, inout RandomSampler rng, out float3 pointOnLight_WS, out float lightPdf, out uint lightIdx)
{
    lightIdx = areaLightSamplingStructure[uint(rng.nextFloat() * sceneParams.numAreaLights)];
    const float lightPickPdf = 1.f / sceneParams.numAreaLights;
    const AreaLight light = areaLights[lightIdx];

    const float2 rndSample = rng.nextFloat2();
    const float sqrtRndX = sqrt(rndSample.x);
    const float2 bary2 = float2(1.f - sqrtRndX, sqrtRndX * rndSample.y);
    pointOnLight_WS = bary2.x * light.pos0_WS + bary2.y * light.pos1_WS + (1.f - bary2.x - bary2.y) * light.pos2_WS;
    float lightSamplePdf = light.rcpArea;

    const float r2 = distance2(surfPos_WS, pointOnLight_WS);
    const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);
    lightSamplePdf *= r2 / absCosTheta(-wi_WS, light.normal_WS);
    lightPdf = lightPickPdf * lightSamplePdf;

    return light;
}

struct DirectLightingSample
{
    bool didHitLight;
    float3 wi_WS;
    float3 Le;
    float pdf;

    // for use with RIS
    // TODO: overlap these with pdf somehow?
    float W_Y;
    float p_hat;
};

bool traceToLight(const float3 surfPos_WS, const float3 surfNor_WS, const float3 wi_WS, const float3 pointOnLight_WS, const AreaLight light, out float3 Le)
{
    RayDesc ray;
    ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
    ray.Direction = wi_WS;
    ray.TMin = 0.f;
    ray.TMax = max(0.f, distance(pointOnLight_WS, surfPos_WS) - (2.f * RAY_ORIGIN_OFFSET_EPSILON));

    Payload lightPayload;
    lightPayload.flags = 0;
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_LIGHTS, 0, 0, ray, lightPayload);

    if (bool(lightPayload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        return false;
    }

    const InstanceData instanceData = instanceDatas[light.instanceId];
    const Material material = materials[instanceData.materialIdx];
    Le = material.getEmissiveColor();
    return true;
}

DirectLightingSample sampleDirectLightingUniform(const float3 surfPos_WS, const float3 surfNor_WS, inout RandomSampler rng)
{
    DirectLightingSample result;
    result.didHitLight = false;

    float3 pointOnLight_WS;
    float lightPdf;
    uint lightIdxUnused;
    const AreaLight light = sampleLightUniform(surfPos_WS, rng, pointOnLight_WS, lightPdf, lightIdxUnused);

    result.wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    float3 Le;
    if (!traceToLight(surfPos_WS, surfNor_WS, result.wi_WS, pointOnLight_WS, light, Le))
    {
        return result;
    }

    result.didHitLight = true;
    result.Le = Le;
    result.pdf = lightPdf;

    return result;
}

float lightPdfUniform(const HitInfo hitInfo, const float3 surfPos_WS, const float3 wi_WS)
{
    const InstanceData instanceData = instanceDatas[hitInfo.instanceId];
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + hitInfo.triangleIdx];
    if (perTriData.localAreaLightIdx == LIGHT_ID_INVALID)
    {
        return 0.f;
    }

    const uint areaLightIdx = instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx;
    const AreaLight light = areaLights[areaLightIdx];
    const float lightPickPdf = 1.f / sceneParams.numAreaLights;
    const float r2 = distance2(surfPos_WS, hitInfo.hitPos_WS);
    return lightPickPdf * light.rcpArea * r2 / absCosTheta(-wi_WS, light.normal_WS);
}

[shader("closesthit")]
void ClosestHit_Lights(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.flags |= PAYLOAD_FLAG_DID_HIT;

    // TODO: add payload flag that determines whether to calculate all this stuff (e.g. not necessary for direct lighting shadow rays, but is necessary for RIS BSDF sample)
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    const InstanceData instanceData = instanceDatas[InstanceID()];
    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float4x3 objectToWorldMat = ObjectToWorld4x3();
    const float3 hitPos_OS = v0.pos * bary.x + v1.pos * bary.y + v2.pos * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), objectToWorldMat).xyz;
}
