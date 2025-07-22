#pragma once

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"
#include "util/math.hlsli"

StructuredBuffer<AreaLight> areaLights : REGISTER_T(REGISTER_AREA_LIGHTS, REGISTER_SPACE_BUFFERS);
StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE, REGISTER_SPACE_BUFFERS);

AreaLight pickLightUniform(float rndSample, out float pdf)
{
    const uint lightIdx = areaLightSamplingStructure[uint(rndSample * sceneParams.numAreaLights)];
    pdf = 1.f / sceneParams.numAreaLights;
    return areaLights[lightIdx];
}

float3 samplePointOnLight(AreaLight light, float2 rndSample, out float pdf)
{
    const float sqrtRndX = sqrt(rndSample.x);
    float2 bary2 = float2(1.f - sqrtRndX, sqrtRndX * rndSample.y);
    float3 pointOnLight_WS = bary2.x * light.pos0 + bary2.y * light.pos1 + (1.f - bary2.x - bary2.y) * light.pos2;
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

DirectLightingSample sampleDirectLighting(float3 origin_WS, float3 normal_WS, float3 rndSample)
{
    DirectLightingSample result;

    float lightPickPdf;
    AreaLight light = pickLightUniform(rndSample.x, lightPickPdf);

    float lightSamplePdf;
    float3 pointOnLight_WS = samplePointOnLight(light, rndSample.yz, lightSamplePdf);

    result.wi_WS = normalize(pointOnLight_WS - origin_WS);

    // TODO: store normal in light? figure out better way to do this?
    const float3 lightNormal_WS = normalize(cross(light.pos1 - light.pos0, light.pos2 - light.pos0));
    const float cosTheta = dot(lightNormal_WS, -result.wi_WS);
    const float r2 = distance2(origin_WS, pointOnLight_WS);
    lightSamplePdf *= r2 / cosTheta;

    RayDesc ray;
    ray.Origin = origin_WS + 0.001f * normal_WS;
    ray.Direction = result.wi_WS;
    ray.TMin = 0.f;
    ray.TMax = 10000.f;

    Payload lightPayload;
    lightPayload.materialId = MATERIAL_ID_INVALID;
    // TODO: figure out why HITGROUP_LIGHTS doesn't work here
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, lightPayload);

    if (lightPayload.materialId == MATERIAL_ID_INVALID || lightPayload.hitInfo.instanceId != light.instanceId || lightPayload.hitInfo.triangleIdx != light.triangleIdx)
    {
        result.didHitLight = false;
        return result;
    }

    result.didHitLight = true;
    const Material material = materials[lightPayload.materialId];
    result.Le = material.emissiveStrength * material.emissiveColor;
    result.pdf = lightPickPdf * lightSamplePdf;

    return result;
}

[shader("closesthit")]
void ClosestHit_Lights(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    const InstanceData instanceData = instanceDatas[InstanceID()];
    payload.materialId = instanceData.materialId;
}
