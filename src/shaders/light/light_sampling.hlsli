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
// lightBary2 holds the sampled point's weights for the light triangle's first two vertices.
void sampleAreaLightPoint(const AreaLight light,
                          const float3 surfPos_WS,
                          inout RandomNumberGenerator rng,
                          out float3 pointOnLight_WS,
                          out float2 lightBary2,
                          out float3 wi_WS,
                          out float lightSamplePdf)
{
    const float2 rndSample = rng.nextFloat2();
    const float sqrtRndX = sqrt(rndSample.x);
    const float2 bary2 = float2(1.f - sqrtRndX, sqrtRndX * rndSample.y);
    lightBary2 = bary2;
    pointOnLight_WS = bary2.x * light.pos0_WS + bary2.y * light.pos1_WS + (1.f - bary2.x - bary2.y) * light.pos2_WS;
    pointOnLight_WS += instanceDatas[light.instanceId].transformOffset - cameraParams.globalInstanceOffset;

    float3 lightNor_WS;
    float lightArea;
    getLightNormalAndArea(light, lightNor_WS, lightArea);

    wi_WS = normalize(pointOnLight_WS - surfPos_WS);
    const float r2 = distance2(surfPos_WS, pointOnLight_WS);
    lightSamplePdf = r2 / (absCosTheta(-wi_WS, lightNor_WS) * lightArea);
}

AreaLight sampleLightUniform(const float3 surfPos_WS,
                             inout RandomNumberGenerator rng,
                             out float3 pointOnLight_WS,
                             out float2 lightBary2,
                             out float lightPdf,
                             out uint lightIdx)
{
    lightIdx = areaLightSamplingStructure[uint(rng.nextFloat() * sceneParams.numAreaLights)];
    const float lightPickPdf = 1.f / sceneParams.numAreaLights;
    const AreaLight light = areaLights[lightIdx];

    float3 wi_WS;
    float lightSamplePdf;
    sampleAreaLightPoint(light, surfPos_WS, rng, pointOnLight_WS, lightBary2, wi_WS, lightSamplePdf);
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

// Occlusion-style shadow ray: TMax stops just short of the light's plane, so any committed
// hit means the light is occluded — no closest hit shader or hit-identity check needed.
// The anyhit shader still runs on non-opaque geometry, preserving passthrough tint and
// water entry/exit tracking for absorption. Le is evaluated from the sampled point's
// barycentrics rather than a closest hit.
bool traceToLight(const float3 surfPos_WS,
                  const float3 surfNor_WS,
                  const float3 wi_WS,
                  const float3 pointOnLight_WS,
                  const float2 lightBary2,
                  const AreaLight light,
                  const RayCone rayCone,
                  const bool canPassthrough,
                  const bool startUnderwater,
                  inout RandomNumberGenerator rng,
                  out float3 Le)
{
    const float lightDistance = distance(surfPos_WS, pointOnLight_WS);

    RayDesc ray;
    setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, wi_WS, false /*faceforwardNormal*/);
    ray.TMin = 0.f;

    // The origin offset shifts the ray parallel to itself, so it crosses the light's plane
    // earlier than lightDistance at oblique angles — TMax must stop short of that plane
    // crossing, not of lightDistance, or the light triangle itself gets committed as an
    // occluder at grazing angles.
    float3 lightNor_WS;
    float lightArea;
    getLightNormalAndArea(light, lightNor_WS, lightArea);
    const float tLightPlane =
        dot(lightNor_WS, pointOnLight_WS - ray.Origin) / dot(lightNor_WS, wi_WS);
    ray.TMax = tLightPlane - rayOriginOffsetEpsilon(pointOnLight_WS);
    if (ray.TMax <= ray.TMin)
    {
        return false;
    }

    Payload lightPayload;
    lightPayload.flags =
        PAYLOAD_FLAG_DID_HIT |
        (canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0) |
        (startUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0);
    lightPayload.pathWeight = float3(1.f, 1.f, 1.f);
    lightPayload.rng = rng;
    lightPayload.waterEntryT = startUnderwater ? 0.f : RAY_DEFAULT_TMAX;
    lightPayload.waterExitT = RAY_DEFAULT_TMAX;
    lightPayload.rayCone = rayCone;
    const uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
    TraceRay(raytracingAcs, rayFlags, 0xFF, HITGROUP_LIGHTS, 0, 0, ray, lightPayload);

    if (bool(lightPayload.flags & PAYLOAD_FLAG_DID_HIT)) // only the miss shader clears this
    {
        return false;
    }

    const Material material = materials[light.materialIdx];
    const float3 passthroughAbsorption = computePassthroughAbsorption(lightPayload, lightDistance);
    const InstanceData lightInstanceData = instanceDatas[light.instanceId];
    const PerTriangleData lightPerTriData =
        perTriDatas[lightInstanceData.perTriDatasBufferOffset + light.triangleIdx];

    Vertex v0, v1, v2;
    loadVertsFromInstance(lightInstanceData, light.triangleIdx, v0, v1, v2);
    const float2 uv =
        lightBary2.x * v0.uv + lightBary2.y * v1.uv + (1.f - lightBary2.x - lightBary2.y) * v2.uv;

    const float coneWidth = getRayConeWidthAtDistance(rayCone, lightDistance);
    // Untinted because this ctx is only used for emission, which is never tinted
    const TexSampleCtx texCtx = makeUntintedTexSampleCtx(computeMipLevel(coneWidth), lightPerTriData.texArraySliceIdx);
    Le = getMaterialEmissiveColor(material, uv, texCtx) * lightPayload.pathWeight * passthroughAbsorption;
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
    float2 lightBary2;
    float lightPdf;
    uint lightIdx;
    const AreaLight light = sampleLightUniform(surfPos_WS, rng, pointOnLight_WS, lightBary2, lightPdf, lightIdx);

    result.pointOnLight_WS = pointOnLight_WS;
    result.wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    float3 Le;
    const bool didHitLight = traceToLight(
        surfPos_WS, surfNor_WS, result.wi_WS, pointOnLight_WS, lightBary2, light, rayCone, canPassthrough, startUnderwater, rng, Le);
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
