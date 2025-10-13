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
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbuffer : REGISTER_T(PT_REGISTER_GBUFFER, PT_REGISTER_SPACE);
RWStructuredBuffer<float4> pathTracingRawBuffer : REGISTER_U(PT_REGISTER_PATH_TRACING_RAW_BUFFER, PT_REGISTER_SPACE);

float powerHeuristic(const float pdfA, const float pdfB)
{
    const float pdfA2 = pdfA * pdfA;
    const float pdfB2 = pdfB * pdfB;
    return pdfA2 / (pdfA2 + pdfB2);
}

void pathTraceRay(inout Payload payload, bool isFirstSample)
{
    const uint pathSplitIdx = getPathSplitIdx();

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(payload.pixelIdx);

    if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialIdx == MATERIAL_IDX_INVALID)
    {
        return;
    }

    for (uint pathDepth = 0; pathDepth < renderParams.maxPathDepth; ++pathDepth)
    {
        Material surfMaterial = materials[payload.materialIdx];

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to sample it twice and multiply by Fresnel reflectance
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

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            return;
        }

        if (isFirstSample && pathDepth == 0 && surfBsdfSample.wasSpecular)
        {
            payload.specularHitDistance = distance(surfPos_WS, payload.hitInfo.hitPos_WS);
        }

        if (renderParams.enableMis == 1)
        {
            const Material hitMaterial = materials[payload.materialIdx];
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
    gbufferPayload.specularHitDistance = 0;

    const uint pathSplitIdx = getPathSplitIdx();

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint sampleIdx = 0; sampleIdx < renderParams.numSamplesPerPixel; ++sampleIdx)
    {
        Payload payload = gbufferPayload;
        payload.rng = initRandomSampler4(uint4(constantParams.rngSeed + pathSplitIdx, linearPixelIdx, sampleIdx, renderParams.frameNumber));

        const bool isFirstSample = (sampleIdx == 0);
        pathTraceRay(payload, isFirstSample);

        accumulatedColor += payload.pathColor;

        if (isFirstSample && pathSplitIdx == 0)
        {
            RWTexture2D<float2> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
            specularHitDistanceTarget[pixelIdx] = payload.specularHitDistance;
        }
    }

    const float3 colorPreTonemap = accumulatedColor / renderParams.numSamplesPerPixel;

    const uint writePixelIdx = linearPixelIdx * (renderParams.enablePathSplitting ? 2 : 1) + pathSplitIdx;
    pathTracingRawBuffer[writePixelIdx] = float4(colorPreTonemap, 1);
}
