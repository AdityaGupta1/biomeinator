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

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "global_params.hlsli"
#include "light_sampling.hlsli"
#include "materials.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"
#include "restir.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbuffer : REGISTER_T(PT_REGISTER_GBUFFER, PT_REGISTER_SPACE);
RWStructuredBuffer<float4> pathTracingRawBuffer : REGISTER_U(PT_REGISTER_PATH_TRACING_RAW_BUFFER, PT_REGISTER_SPACE);

float balanceHeuristic(const float pdfA, const float pdfB)
{
    return debugParams.debugBool0 == 0 ? (pdfA / (pdfA + pdfB)) : 0.5f;
}

void pathTraceRay(inout Payload payload, bool isFirstSample)
{
    const uint pathSplitIdx = getPathSplitIdx();

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(payload.pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialIdx == MATERIAL_IDX_INVALID)
    {
        return;
    }

    for (uint pathDepth = 0; pathDepth < renderParams.maxPathDepth; ++pathDepth)
    {
        Material surfMaterial = materials[payload.materialIdx];

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and multiply by Fresnel reflectance
        if ((pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
        {
            payload.pathColor += payload.pathWeight * surfMaterial.getEmissiveColor();
        }

        const float3 wo_WS = -ray.Direction;
        const float3 surfNor_WS = faceforward(payload.hitInfo.hitNor_WS, wo_WS);

        if (pathDepth == 0 && renderParams.enablePathSplitting)
        {
            if (shouldSplitMaterial(surfMaterial))
            {
                surfMaterial = getSplitMaterial(surfMaterial, surfNor_WS, wo_WS, pathSplitIdx, payload.pathWeight);
            }
            else if (pathSplitIdx == 1)
            {
                return;
            }
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

        const float3 surfPos_WS = payload.hitInfo.hitPos_WS;

        if (renderParams.enableMis == 1)
        {
            if (!surfMaterial.isOnlySpecular())
            {
                DirectLightingSample lightSample;
                if (renderParams.enableRis == 1)
                {
                    lightSample = sampleDirectLightingRis(surfPos_WS, surfNor_WS, payload.rng);
                }
                else
                {
                    lightSample = sampleDirectLightingUniform(surfPos_WS, surfNor_WS, payload.rng);
                }

                if (lightSample.didHitLight)
                {
                    // TODO: reuse fresnel reflectance from evaluateBsdf() in bsdfPdf()
                    const float3 bsdfVal = evaluateBsdf(
                        surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS, true /*calculateFresnelReflectance*/);
                    const float lightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, lightSample.wi_WS, surfNor_WS);

                    float3 contribution = payload.pathWeight * bsdfVal * absCosTheta(lightSample.wi_WS, surfNor_WS) * lightSample.Le;

                    if (renderParams.enableRis)
                    {
                        const float misWeight = balanceHeuristic(lightSample.p_hat, lightSampleBsdfPdf);
                        contribution *= misWeight * lightSample.W_Y;
                    }
                    else
                    {
                        const float misWeight = balanceHeuristic(lightSample.pdf, lightSampleBsdfPdf);
                        contribution *= misWeight / lightSample.pdf;
                    }

                    payload.pathColor += contribution;
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

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            return;
        }

        if (isFirstSample && pathDepth == 0 && surfBsdfSample.wasSpecular)
        {
            RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
            specularHitDistanceTarget[payload.pixelIdx] = distance(surfPos_WS, payload.hitInfo.hitPos_WS);

            const float3 secondBounceNor_WS = faceforward(payload.hitInfo.hitNor_WS, -ray.Direction);
            RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
            normalsAndRoughnessTarget[payload.pixelIdx].xyz = secondBounceNor_WS;
        }

        if (renderParams.enableMis == 1)
        {
            const Material hitMaterial = materials[payload.materialIdx];
            if (hitMaterial.hasEmission() && !surfBsdfSample.wasSpecular)
            {
                float bsdfSampleLightPdf; // not a pdf if using ReSTIR
                // TODO: extract this block into a function getBsdfSampleLightPdf()
                {
                    const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
                    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
                    if (perTriData.localAreaLightIdx == LIGHT_ID_INVALID)
                    {
                        bsdfSampleLightPdf = 0.f;
                    }
                    else
                    {
                        const uint areaLightIdx = instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx;
                        const AreaLight light = areaLights[areaLightIdx];

                        if (renderParams.enableRis == 1)
                        {
                            bsdfSampleLightPdf = risTargetFunction(light, surfPos_WS, surfNor_WS, payload.hitInfo.hitPos_WS);
                        }
                        else
                        {
                            bsdfSampleLightPdf = lightPdfUniform(light, surfPos_WS, payload.hitInfo.hitPos_WS, ray.Direction);
                        }
                    }
                }

                const float misWeight = balanceHeuristic(surfBsdfSample.pdf, bsdfSampleLightPdf);
                payload.pathWeight *= misWeight;
            }
            // if BSDF sampling didn't hit a light, lightPdf = 0 (I think) so misWeight = 1
        }
    }
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = getPixelIdx();
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const GbufferData gbufferData = gbuffer[linearPixelIdx];
    Payload gbufferPayload;
    gbufferPayload.hitInfo = gbufferData.hitInfo;
    gbufferPayload.materialIdx = gbufferData.materialIdx;
    gbufferPayload.flags = gbufferData.payloadFlags;
    gbufferPayload.pathWeight = float3(1, 1, 1);
    gbufferPayload.pathColor = float3(0, 0, 0);
    gbufferPayload.pixelIdx = pixelIdx;

    const uint pathSplitIdx = getPathSplitIdx();

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint sampleIdx = 0; sampleIdx < renderParams.numSamplesPerPixel; ++sampleIdx)
    {
        Payload payload = gbufferPayload;
        payload.rng = initRandomSampler4(uint4(constantParams.rngSeed + pathSplitIdx, linearPixelIdx, sampleIdx, renderParams.frameNumber));

        const bool isFirstSample = (sampleIdx == 0);
        pathTraceRay(payload, isFirstSample);

        accumulatedColor += payload.pathColor;
    }

    const float3 colorPreTonemap = accumulatedColor / renderParams.numSamplesPerPixel;

    const uint writePixelIdx = linearPixelIdx * (renderParams.enablePathSplitting ? 2 : 1) + pathSplitIdx;
    pathTracingRawBuffer[writePixelIdx] = float4(colorPreTonemap, 1);
}
