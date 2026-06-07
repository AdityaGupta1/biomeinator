// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#define HITGROUP_LIGHTS GBUFFER_HITGROUP_LIGHTS

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "light/ris.hlsli"
#include "common/light_tree_sampling.hlsli"
#include "materials/materials.hlsli"
#include "util/color.hlsli"
#include "util/rng.hlsli"

RWStructuredBuffer<GbufferData> gbufferOut : REGISTER_U(GBUFFER, GBUFFER_OUT);
RWStructuredBuffer<RisSample> risSamplesOut : REGISTER_U(GBUFFER, RIS_SAMPLES_OUT);

// motion is in uv space, not pixel space
float2 calculateMotionFromPos(const float3 pos_WS)
{
    float4 currNdc = mul(cameraParams.worldToClipMat, float4(pos_WS, 1));
    currNdc /= currNdc.w;
    float4 prevNdc = mul(cameraParams.worldToPrevClipMat, float4(pos_WS, 1)); // worldToPrevClipMat accounts for changed globalInstanceOffset
    prevNdc /= prevNdc.w;

    float2 motion = (prevNdc.xy - currNdc.xy) / 2.f;
    motion.y = -motion.y;
    return motion;
}

void outputGuideBuffers(const Payload payload, const RayDesc ray)
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;

    float linearDepth = cameraParams.farPlane;
    float3 motionHitPos_WS;
    float3 hitNor_WS = 0.f;
    float roughness = 0.f;
    float3 specularAlbedo = 0.f;

    // diffuse albedo is written in path_tracing.rgs.hlsl so it can be modulated by specular bounces

    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        linearDepth = distance(ray.Origin, payload.hitInfo.hitPos_WS);

        motionHitPos_WS = payload.hitInfo.hitPos_WS;
        hitNor_WS = payload.hitInfo.hitNor_WS;

        // TODO: eventually set roughness

        if (payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const Material surfMaterial = getMaterialFromPayload(payload);

            if (surfMaterial.hasGlossyReflection())
            {
                const float alpha = roughness * roughness;
                const float nDotV = dot(hitNor_WS, -ray.Direction);
                specularAlbedo = calculateDlssSpecularAlbedo(surfMaterial.glossyReflectionTint, alpha, nDotV);
            }
            else
            {
                roughness = 1.f; // for now, roughness is 0.f for all materials that have glossy reflection and 1.f otherwise
            }
        }
    }
    else
    {
        motionHitPos_WS = evalRayPos(ray, cameraParams.farPlane);
        hitNor_WS = normalize(-ray.Direction);
    }

    RWTexture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.uav.linearDepthTargetIdx];
    linearDepthTarget[pixelIdx] = linearDepth;

    RWTexture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.uav.motionTargetIdx];
    motionTarget[pixelIdx] = calculateMotionFromPos(motionHitPos_WS);

    RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
    normalsAndRoughnessTarget[pixelIdx].xyzw = float4(hitNor_WS, roughness);

    RWTexture2D<float4> specularAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.specularAlbedoTargetIdx];
    specularAlbedoTarget[pixelIdx] = float4(specularAlbedo, 1);

    RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
    specularHitDistanceTarget[pixelIdx] = 0; // will be overwritten in path tracing pass
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    RayDesc ray;
    ray.Origin = cameraParams.pos_WS;
    ray.Direction = getPrimaryRayDirection(pixelIdx);
    ray.TMin = 0.001f;
    ray.TMax = RAY_DEFAULT_TMAX;

    Payload payload;
    payload.materialIdx = MATERIAL_IDX_INVALID;
    payload.flags = (sceneParams.cameraUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0) | PAYLOAD_FLAG_IS_GBUFFER;
    payload.rng = initRng(constantParams.rngSeed, 123909203, linearPixelIdx, renderParams.frameNumber);
    payload.waterEntryT = RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone.width = 0.f;
    payload.rayCone.angle = getRayConePixelAngle();

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, GBUFFER_HITGROUP_PRIMARY, 0, 0, ray, payload);

    outputGuideBuffers(payload, ray);

    GbufferData outGbufferData;
    outGbufferData.hitInfo = payload.hitInfo;
    outGbufferData.materialIdx = payload.materialIdx;
    outGbufferData.payloadFlags = payload.flags;
    outGbufferData.pad0 = outGbufferData.pad1 = 0; // necessary since we're writing to a UAV
    gbufferOut[linearPixelIdx] = outGbufferData;

    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    if (samplingMode == SamplingMode::RESTIR)
    {
        RisSample risSample;
        risSample.lightIdx = LIGHT_IDX_INVALID;
        risSample.pointOnLight_WS = 0.f;
        risSample.W = 0.f;
        risSample.p_hat = 0.f;
        risSample.confidence = 0;
        risSample.pad0 = 0;

        if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const Material surfMaterial = materials[payload.materialIdx];
            if (surfMaterial.canScatter() && !surfMaterial.isDelta())
            {
                const float3 wo_WS = -ray.Direction;
                const float3 surfNor_WS = faceforward(payload.hitInfo.hitNor_WS, wo_WS);
                const float3 surfPos_WS = payload.hitInfo.hitPos_WS;

                RandomNumberGenerator rng = initRng(constantParams.rngSeed, 6831107, linearPixelIdx, renderParams.frameNumber);

                uint pickedLightIdx;
                float pdfSelect;
                const bool gotLight = selectLightFromSubtree(0u, surfPos_WS, surfNor_WS, rng, pickedLightIdx, pdfSelect);
                if (gotLight)
                {
                    const AreaLight light = areaLights[pickedLightIdx];

                    float3 pointOnLight_WS, wi_WS;
                    float lightSamplePdf;
                    sampleAreaLightPoint(light, surfPos_WS, rng, pointOnLight_WS, wi_WS, lightSamplePdf);

                    const float p_hat = risTargetFunction(light, pointOnLight_WS, surfPos_WS, surfNor_WS);
                    const float q = pdfSelect * lightSamplePdf;

                    risSample.lightIdx = pickedLightIdx;
                    risSample.pointOnLight_WS = pointOnLight_WS;
                    risSample.W = (p_hat > 0.f && q > 0.f) ? (1.f / q) : 0.f;
                    risSample.p_hat = p_hat;
                    risSample.confidence = 1;

                    if (renderParams.restirDoVisibilityCheck == 1)
                    {
                        const DirectLightingSample lightSample = evaluateRisSample(
                            risSample, surfPos_WS, surfNor_WS, payload.rayCone, false, false, rng);
                        if (!lightSample.didHitLight)
                        {
                            risSample.W = 0.f;
                        }
                    }
                }
            }
        }

        risSamplesOut[linearPixelIdx] = risSample;
    }
}
