// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "materials/materials.hlsli"
#include "util/math.hlsli"

StructuredBuffer<AreaLight> areaLights : REGISTER_T(RT, AREA_LIGHTS);
StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(RT, AREA_LIGHT_SAMPLING_STRUCTURE);

void getLightNormalAndArea(const AreaLight light, out float3 lightNor_WS, out float area)
{
    const float3 crossVec = cross(light.pos0_WS - light.pos1_WS, light.pos2_WS - light.pos0_WS);
    lightNor_WS = normalize(crossVec);
    area = length(crossVec) * 0.5f;
}

// Samples a point uniformly over the triangle area of `light` and the resulting
// area-to-solid-angle pdf at `surfPos_WS`. Does NOT include the per-light pick
// probability — callers multiply that in based on their selection scheme.
void sampleAreaLightPoint(const AreaLight light,
                          const float3 surfPos_WS,
                          inout RandomNumberGenerator rng,
                          out float3 pointOnLight_WS,
                          out float3 wi_WS,
                          out float lightSamplePdf)
{
    const float2 rndSample = rng.nextFloat2();
    const float sqrtRndX = sqrt(rndSample.x);
    const float2 bary2 = float2(1.f - sqrtRndX, sqrtRndX * rndSample.y);
    pointOnLight_WS = bary2.x * light.pos0_WS + bary2.y * light.pos1_WS + (1.f - bary2.x - bary2.y) * light.pos2_WS;
    pointOnLight_WS += instanceDatas[light.instanceId].transformOffset - cameraParams.globalInstanceOffset;

    float3 lightNor_WS;
    float lightArea;
    getLightNormalAndArea(light, lightNor_WS, lightArea);

    wi_WS = normalize(pointOnLight_WS - surfPos_WS);
    const float r2 = distance2(surfPos_WS, pointOnLight_WS);
    lightSamplePdf = r2 / (absCosTheta(-wi_WS, lightNor_WS) * lightArea);
}

AreaLight sampleLightUniform(const float3 surfPos_WS, inout RandomNumberGenerator rng, out float3 pointOnLight_WS, out float lightPdf, out uint lightIdx)
{
    lightIdx = areaLightSamplingStructure[uint(rng.nextFloat() * sceneParams.numAreaLights)];
    const float lightPickPdf = 1.f / sceneParams.numAreaLights;
    const AreaLight light = areaLights[lightIdx];

    float3 wi_WS;
    float lightSamplePdf;
    sampleAreaLightPoint(light, surfPos_WS, rng, pointOnLight_WS, wi_WS, lightSamplePdf);
    lightPdf = lightPickPdf * lightSamplePdf;

    return light;
}

struct DirectLightingSample
{
    uint lightIdx;
    bool didHitLight;
    float3 pointOnLight_WS;
    float3 wi_WS;
    float3 Le;
    float pdfOrW_Y;
};

#ifdef HITGROUP_LIGHTS
bool traceToLight(const float3 surfPos_WS,
                  const float3 surfNor_WS,
                  const float3 wi_WS,
                  const float3 pointOnLight_WS,
                  const AreaLight light,
                  const RayCone rayCone,
                  const bool canPassthrough,
                  const bool startUnderwater,
                  inout RandomNumberGenerator rng,
                  out float3 Le)
{
    RayDesc ray;
    setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, wi_WS, false /*faceforwardNormal*/);
    ray.TMin = 0.f;
    ray.TMax = distance(surfPos_WS, pointOnLight_WS) + 2 * rayOriginOffsetEpsilon(surfPos_WS);

    Payload lightPayload;
    lightPayload.flags =
        (canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0) |
        (startUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0);
    lightPayload.pathWeight = float3(1.f, 1.f, 1.f);
    lightPayload.rng = rng;
    lightPayload.waterEntryT = startUnderwater ? 0.f : RAY_DEFAULT_TMAX;
    lightPayload.waterExitT = RAY_DEFAULT_TMAX;
    lightPayload.rayCone = rayCone;
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_LIGHTS, 0, 0, ray, lightPayload);

    if (!bool(lightPayload.flags & PAYLOAD_FLAG_DID_HIT) || lightPayload.hitInfo.instanceId != light.instanceId || lightPayload.hitInfo.triangleIdx != light.triangleIdx)
    {
        return false;
    }

    const Material material = materials[light.materialIdx];
    const float3 passthroughAbsorption = computePassthroughAbsorption(lightPayload, distance(ray.Origin, lightPayload.hitInfo.hitPos_WS));
    const float lightHitDistance = distance(ray.Origin, lightPayload.hitInfo.hitPos_WS);
    // Untinted because this ctx is only used for emission, which is never tinted
    const TexSampleCtx texCtx = makeUntintedTexSampleCtx(
        computeMipLevel(getRayConeWidthAtDistance(lightPayload.rayCone, lightHitDistance)),
        perTriDatas[instanceDatas[lightPayload.hitInfo.instanceId].perTriDatasBufferOffset + lightPayload.hitInfo.triangleIdx].texArraySliceIdx);
    Le = getMaterialEmissiveColor(material, lightPayload.hitInfo.uv, texCtx) * lightPayload.pathWeight * passthroughAbsorption;
    return true;
}

DirectLightingSample sampleDirectLightingUniform(const float3 surfPos_WS,
                                                 const float3 surfNor_WS,
                                                 const RayCone rayCone,
                                                 const bool canPassthrough,
                                                 const bool startUnderwater,
                                                 inout RandomNumberGenerator rng)
{
    DirectLightingSample result;
    result.didHitLight = false;

    float3 pointOnLight_WS;
    float lightPdf;
    uint lightIdx;
    const AreaLight light = sampleLightUniform(surfPos_WS, rng, pointOnLight_WS, lightPdf, lightIdx);

    result.pointOnLight_WS = pointOnLight_WS;
    result.wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    float3 Le;
    const bool didHitLight = traceToLight(
        surfPos_WS, surfNor_WS, result.wi_WS, pointOnLight_WS, light, rayCone, canPassthrough, startUnderwater, rng, Le);
    if (!didHitLight)
    {
        return result;
    }

    result.lightIdx = lightIdx;
    result.didHitLight = true;
    result.Le = Le;
    result.pdfOrW_Y = lightPdf;

    return result;
}
#endif

// Decodes the global area-light index of the triangle the hit landed on, or
// LIGHT_IDX_INVALID if the triangle is not emissive.
uint getAreaLightIdxFromHit(const HitInfo hitInfo)
{
    const InstanceData instanceData = instanceDatas[hitInfo.instanceId];
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + hitInfo.triangleIdx];
    if (perTriData.localAreaLightIdx == LIGHT_IDX_INVALID)
    {
        return LIGHT_IDX_INVALID;
    }
    return instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx;
}

float lightPdfUniform(const HitInfo hitInfo, const float3 surfPos_WS, const float3 wi_WS)
{
    const uint areaLightIdx = getAreaLightIdxFromHit(hitInfo);
    if (areaLightIdx == LIGHT_IDX_INVALID)
    {
        return 0.f;
    }

    const AreaLight light = areaLights[areaLightIdx];

    float3 lightNor_WS;
    float lightArea;
    getLightNormalAndArea(light, lightNor_WS, lightArea);

    const float lightPickPdf = 1.f / sceneParams.numAreaLights;
    const float r2 = distance2(surfPos_WS, hitInfo.hitPos_WS);
    return lightPickPdf * r2 / (absCosTheta(-wi_WS, lightNor_WS) * lightArea);
}

[shader("closesthit")]
void ClosestHit_Lights(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.flags |= PAYLOAD_FLAG_DID_HIT;

    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    const InstanceData instanceData = instanceDatas[InstanceID()];

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float4x3 objectToWorldMat = ObjectToWorld4x3();
    const float3 hitPos_OS = v0.pos_OS * bary.x + v1.pos_OS * bary.y + v2.pos_OS * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), objectToWorldMat).xyz;

    payload.hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
}
