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

#define HITGROUP_LIGHTS PT_HITGROUP_LIGHTS

#include "nvapi_includes.hlsli"

#include "dome_light.hlsli"
#include "global_params.hlsli"
#include "light_sampling.hlsli"
#include "materials.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"
#include "ris.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbufferIn : REGISTER_T(PT, GBUFFER_IN);
RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);

float balanceHeuristic(const float pdfA, const float pdfB)
{
    return pdfA / (pdfA + pdfB);
}

void pathTraceRay(inout Payload payload)
{
    const uint2 pixelIdx = getPixelIdx();

    const uint pathSplitIdx = getPathSplitIdx();
    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRis = (samplingMode == SamplingMode::RIS);
    const bool doMis = (samplingMode == SamplingMode::MIS || useRis);

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        payload.pathColor = getDomeLightColor(ray.Direction);
        return;
    }
    else if (payload.materialIdx == MATERIAL_IDX_INVALID)
    {
        return;
    }

    bool previousWasSpecular = false;
    bool hasEncounteredNonDeltaSurface = false;
    float prevBsdfPdf = 0.f;

    if (sceneParams.voxelMode == 1 && debugParams.colorChunks == 1)
    {
        float3 surfPos_WS = payload.hitInfo.hitPos_WS;
        surfPos_WS.xz += cameraParams.globalInstanceOffset.xz;
        const int2 chunkPosBlocksXZ_WS = int2(floor(surfPos_WS.xz / 16.f)); // should be chunkSizeXZ instead of 16.f but whatever
        const float3 chunkColor = (chunkPosBlocksXZ_WS.x + chunkPosBlocksXZ_WS.y /*z*/) % 2 == 0 ? float3(1.f, 0.5f, 0.5f) : float3(0.5f, 1.f, 1.f);
        payload.pathWeight *= chunkColor;
    }

    for (uint pathDepth = 0; pathDepth < renderParams.maxPathDepth; ++pathDepth)
    {
        Material surfMaterial = getMaterialFromPayload(payload);

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and multiply by Fresnel reflectance
        // In RIS mode, only include emission if this is the first bounce (pathDepth == 0) or the previous event was a delta event (specular)
        if ((pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
        {
            payload.pathColor += payload.pathWeight * getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv);
        }

        const float3 wo_WS = -ray.Direction;
        const float3 surfNor_WS = payload.hitInfo.hitNor_WS;

        if (pathDepth == 0 && bool(renderParams.doPathSplitting))
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

        if (bool(renderParams.refractionIndirectPassthrough) && hasEncounteredNonDeltaSurface && surfMaterial.hasGlossyTransmission())
        {
            payload.pathWeight *= getMaterialBaseColor(surfMaterial, payload.hitInfo.uv).rgb;
            setRayOriginAndDirection(ray, payload.hitInfo.hitPos_WS, surfNor_WS, ray.Direction, true /*faceforwardNormal*/);
            ray.TMin = 0.f;
            ray.TMax = RAY_DEFAULT_TMAX;
            payload.flags = 0;
            TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);

            if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
            {
                if (doMis)
                {
                    const float bsdfSampleDomeLightPdf = domeLightPdf(ray.Direction, surfNor_WS);
                    payload.pathWeight *= prevBsdfPdf / (prevBsdfPdf + bsdfSampleDomeLightPdf);
                }
                payload.pathColor += payload.pathWeight * getDomeLightColor(ray.Direction);
                return;
            }
            if (payload.materialIdx == MATERIAL_IDX_INVALID) { return; }
            continue;
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

        const bool isNonDeltaSurface = !surfMaterial.isDelta();

        uint coherenceHint = (isNonDeltaSurface && surfMaterial.canScatter()) ? (1 << 0) : 0;
        uint numCoherenceHintBits = 1;
        if (useRis)
        {
            coherenceHint |= (pathDepth == 0 ? (1 << 1) : 0);
            ++numCoherenceHintBits;
        }

        NvReorderThread(coherenceHint, numCoherenceHintBits);

        if (doMis && surfMaterial.canScatter() && isNonDeltaSurface)
        {
            // ------------------------------
            // sample area lights
            // ------------------------------

            DirectLightingSample lightSample;
            if (useRis)
            {
                const bool isFirstNonDeltaSurface = !hasEncounteredNonDeltaSurface;
                bool isBsdfSampleUnused;
                const RisSample risSample = generateDirectLightingRisSample(surfPos_WS, surfNor_WS, surfMaterial, payload.hitInfo.uv, wo_WS, isFirstNonDeltaSurface, payload.rng, isBsdfSampleUnused);
                lightSample = evaluateRisSample(risSample, surfPos_WS, surfNor_WS, bool(renderParams.refractionIndirectPassthrough)); // this checks if risSample.lightIdx == LIGHT_IDX_INVALID
            }
            else
            {
                lightSample = sampleDirectLightingUniform(surfPos_WS, surfNor_WS, bool(renderParams.refractionIndirectPassthrough), payload.rng);
            }

            if (lightSample.didHitLight)
            {
                // no need to consider dome light pdf because dome light sampling can't hit area lights

                const float3 bsdfVal = evaluateBsdf(
                    surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS);

                float3 contribution = payload.pathWeight * bsdfVal * absCosTheta(lightSample.wi_WS, surfNor_WS) * lightSample.Le;

                const float lightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, lightSample.wi_WS, surfNor_WS);
                if (useRis)
                {
                    const float W = lightSample.pdfOrW_Y;

                    const AreaLight light = areaLights[lightSample.lightIdx];

                    float3 lightNor_WS;
                    float lightArea;
                    getLightNormalAndArea(light, lightNor_WS, lightArea);

                    // TODO: use lightPdfUniform function?
                    const float r2 = distance2(surfPos_WS, lightSample.pointOnLight_WS);
                    const float lightPdf = r2 / (absCosTheta(-lightSample.wi_WS, lightNor_WS) * lightArea * sceneParams.numAreaLights);

                    const float balanceHeuristicWeight = lightPdf / (lightPdf + lightSampleBsdfPdf);
                    contribution *= W * balanceHeuristicWeight;
                }
                else
                {
                    const float lightPdf = lightSample.pdfOrW_Y;
                    const float balanceHeuristicDenominator = lightPdf + lightSampleBsdfPdf;

                    contribution /= balanceHeuristicDenominator; // light pdf in balance heuristic numerator cancels out with divide by pdf
                }

                payload.pathColor += contribution;
            }

            // ------------------------------
            // sample dome light
            // ------------------------------

            if (sceneParams.voxelMode == 1)
            {
                DomeLightSample domeLightSample = sampleDomeLight(surfPos_WS, surfNor_WS, bool(renderParams.refractionIndirectPassthrough), payload.rng);
                if (domeLightSample.didReachDomeLight)
                {
                    // no need to consider area light pdf because area light sampling can't hit dome light

                    const float3 bsdfVal = evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, domeLightSample.wi_WS, surfNor_WS);

                    float3 contribution = payload.pathWeight * bsdfVal * absCosTheta(domeLightSample.wi_WS, surfNor_WS) * domeLightSample.Le;

                    const float domeLightPdf = domeLightSample.pdf;
                    const float domeLightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, domeLightSample.wi_WS, surfNor_WS);
                    const float balanceHeuristicDenominator = domeLightPdf + domeLightSampleBsdfPdf;

                    contribution /= balanceHeuristicDenominator; // dome light pdf in balance heuristic numerator cancels out with divide by pdf

                    payload.pathColor += contribution;
                }
            }
        }

        if (isNonDeltaSurface)
        {
            hasEncounteredNonDeltaSurface = true;
        }

        const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, payload.rng);

        payload.pathWeight *= surfBsdfSample.bsdfValue / surfBsdfSample.pdf;
        if (!surfBsdfSample.wasSpecular)
        {
            payload.pathWeight *= absCosTheta(surfBsdfSample.wi_WS, surfNor_WS);
        }

        setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, surfBsdfSample.wi_WS, true /*faceforwardNormal*/);
        ray.TMin = 0.f;
        ray.TMax = RAY_DEFAULT_TMAX;

        payload.flags = 0;
        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
        {
            if (doMis)
            {
                const float bsdfSampleDomeLightPdf = domeLightPdf(ray.Direction, surfNor_WS); // 0 if !voxelMode
                const float balanceHeuristicWeight = surfBsdfSample.pdf / (surfBsdfSample.pdf + bsdfSampleDomeLightPdf);
                payload.pathWeight *= balanceHeuristicWeight;
            }

            payload.pathColor += payload.pathWeight * getDomeLightColor(ray.Direction);
            return;
        }
        else if (payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            return;
        }

        if (pathDepth == 0 && surfBsdfSample.wasSpecular) // TODO: update to support multiple specular bounces?
        {
            RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
            specularHitDistanceTarget[pixelIdx] = distance(surfPos_WS, payload.hitInfo.hitPos_WS);

            RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
            normalsAndRoughnessTarget[pixelIdx].xyz = payload.hitInfo.hitNor_WS;
        }

        if (doMis)
        {
            // no need to consider dome light pdf here because dome light sampling can't hit area lights

            const Material hitMaterial = getMaterialFromPayload(payload);

            if (hitMaterial.hasEmission() && !surfBsdfSample.wasSpecular)
            {
                const float bsdfSampleLightPdf = lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);

                const float balanceHeuristicWeight = surfBsdfSample.pdf / (surfBsdfSample.pdf + bsdfSampleLightPdf);
                payload.pathWeight *= balanceHeuristicWeight;
            }

            // if BSDF sampling hit something other than a light, lightPdf = 0 so misWeight = 1
        }

        prevBsdfPdf = surfBsdfSample.pdf;
        previousWasSpecular = surfBsdfSample.wasSpecular;
    }
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = getPixelIdx();
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    Payload gbufferPayload;
    gbufferPayload.hitInfo = gbufferData.hitInfo;
    gbufferPayload.materialIdx = gbufferData.materialIdx;
    gbufferPayload.flags = gbufferData.payloadFlags;
    gbufferPayload.pathWeight = float3(1.f, 1.f, 1.f);
    gbufferPayload.pathColor = float3(0.f, 0.f, 0.f);

    const uint pathSplitIdx = getPathSplitIdx();

    Payload payload = gbufferPayload;
    payload.rng = initRng(constantParams.rngSeed, 987654103, linearPixelIdx * (pathSplitIdx + 1), renderParams.frameNumber);

    pathTraceRay(payload);

    const float3 colorPreTonemap = payload.pathColor;
    const uint writePixelIdx = linearPixelIdx * (bool(renderParams.doPathSplitting) ? 2 : 1) + pathSplitIdx;
    if ((AntialiasingMode)renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE && renderParams.accumulatedFrameNumber > 0)
    {
        pathTracingRawBufferOut[writePixelIdx].xyz += colorPreTonemap;
    }
    else
    {
        pathTracingRawBufferOut[writePixelIdx].xyz = colorPreTonemap;
    }
}
