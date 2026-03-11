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
RWStructuredBuffer<float4> ptDiffuseAlbedoRawBufferOut : REGISTER_U(PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);

float balanceHeuristic(const float pdfA, const float pdfB)
{
    return pdfA / (pdfA + pdfB);
}

void pathTraceRay(inout Payload payload, out float3 ptDiffuseAlbedo)
{
    const uint2 pixelIdx = getPixelIdx();

    const uint pathSplitIdx = getPathSplitIdx();
    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRis = (samplingMode == SamplingMode::RIS);
    const bool doMis = (samplingMode == SamplingMode::MIS || useRis);

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    ptDiffuseAlbedo = 0.f;

    const float3 segmentAbsorption = computeSegmentAbsorption(payload, cameraParams.pos_WS, ray.Direction);
    payload.pathWeight *= segmentAbsorption;

    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        payload.pathColor = payload.pathWeight * getDomeLightColor(ray.Direction);
        return;
    }

    if (payload.materialIdx == MATERIAL_IDX_INVALID)
    {
        return;
    }

    // data of last "real" bounce (i.e. not passthrough)
    bool bounceWasSpecular = false;
    float bounceBsdfPdf = 0.f;
    float3 surfPos_WS, surfNor_WS;

    bool hasEncounteredNonDeltaSurface = false;

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
        const float surfMipLevel = computeTerrainMipLevel(payload.rayCone.width);

        const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
        const PerTriangleData perTriData =
            perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
        const bool hitWasWater = bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and multiply by Fresnel reflectance
        // In RIS mode, only include emission if this is the first bounce (pathDepth == 0) or the previous event was a delta event (specular)
        if ((pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
        {
            const float3 emissiveContrib =
                payload.pathWeight * getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, surfMipLevel);
            payload.pathColor += emissiveContrib;

            if (pathDepth == 0)
            {
                ptDiffuseAlbedo += applyReinhard(emissiveContrib);
            }
        }

        const float3 wo_WS = -ray.Direction;

        if (pathDepth == 0 && bool(renderParams.doPathSplitting))
        {
            const bool didSplitMaterial = trySplitMaterial(
                surfMaterial, payload.hitInfo.uv, payload.hitInfo.hitNor_WS, wo_WS, surfMipLevel, pathSplitIdx, payload.pathWeight);
            if (!didSplitMaterial && pathSplitIdx == 1)
            {
                return;
            }
        }

        const bool isLastBounce = pathDepth == renderParams.maxPathDepth - 1;
        if (!surfMaterial.canScatter() || isLastBounce)
        {
            return;
        }

        const bool isDeltaSurface = surfMaterial.isDelta();

        // canPassthrough = has the path encountered a non-delta surface (including this one)
        // isPassthrough = this intersection has glossy transmission and should be passed through
        const bool canPassthrough = bool(renderParams.refractionIndirectPassthrough) && (!isDeltaSurface || hasEncounteredNonDeltaSurface);
        const bool isPassthrough = canPassthrough && surfMaterial.hasGlossyTransmission() && isDeltaSurface;

        // If this is a passthrough "bounce", we don't care about its hit pos/nor and want to instead preserve the last
        // "real" bounce's information. This is important for matching MIS weights with direct light sampling, which
        // traces only one ray and ignores passthrough surfaces in the anyhit shader.
        if (!isPassthrough)
        {
            surfNor_WS = payload.hitInfo.hitNor_WS;
            surfPos_WS = payload.hitInfo.hitPos_WS;
        }

        const uint coherenceHint =
            (pathDepth == 0 ? (1 << 2) : 0) |
            (isPassthrough ? (1 << 1) : 0) |
            (!isDeltaSurface && surfMaterial.canScatter()) ? (1 << 0) : 0;
        NvReorderThread(coherenceHint, 3 /*numCoherenceHintBits*/);

        if (isPassthrough)
        {
            payload.pathWeight *= getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfMipLevel).rgb;
            if (hitWasWater)
            {
                setUnderwaterFromHit(payload, bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT));
            }
            setRayOriginAndDirection(ray, payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS, ray.Direction, true /*faceforwardNormal*/);
            // bounceBsdfPdf and bounceWasSpecular are intentionally preserved from the last real BSDF sample
        }
        else // !isPassthrough
        {
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

            if (doMis && surfMaterial.canScatter() && !isDeltaSurface)
            {
                // ------------------------------
                // sample area lights
                // ------------------------------

                const bool isUnderwater = payload.flags & PAYLOAD_FLAG_UNDERWATER;

                DirectLightingSample lightSample;
                if (useRis)
                {
                    const bool isFirstNonDeltaSurface = !hasEncounteredNonDeltaSurface;
                    bool isBsdfSampleUnused;
                    const RisSample risSample = generateDirectLightingRisSample(surfPos_WS,
                                                                                surfNor_WS,
                                                                                surfMaterial,
                                                                                payload.hitInfo.uv,
                                                                                wo_WS,
                                                                                isFirstNonDeltaSurface,
                                                                                payload.rng,
                                                                                isBsdfSampleUnused);
                    // this checks if risSample.lightIdx == LIGHT_IDX_INVALID
                    lightSample = evaluateRisSample(risSample, surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater, payload.rng);
                }
                else
                {
                    lightSample =
                        sampleDirectLightingUniform(surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater, payload.rng);
                }

                if (lightSample.didHitLight)
                {
                    // no need to consider dome light pdf because dome light sampling can't hit area lights

                    const float3 bsdfVal = evaluateBsdf(
                        surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS, surfMipLevel);

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
                    DomeLightSample domeLightSample =
                        sampleDomeLight(surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater, payload.rng);
                    if (domeLightSample.didReachDomeLight)
                    {
                        // no need to consider area light pdf because area light sampling can't hit dome light

                        const float3 bsdfVal = evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, domeLightSample.wi_WS, surfNor_WS, surfMipLevel);

                        float3 contribution = payload.pathWeight * bsdfVal * absCosTheta(domeLightSample.wi_WS, surfNor_WS) * domeLightSample.Le;

                        const float domeLightPdf = domeLightSample.pdf;
                        const float domeLightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, domeLightSample.wi_WS, surfNor_WS);
                        const float balanceHeuristicDenominator = domeLightPdf + domeLightSampleBsdfPdf;

                        contribution /= balanceHeuristicDenominator; // dome light pdf in balance heuristic numerator cancels out with divide by pdf

                        payload.pathColor += contribution;
                    }
                }
            }

            if (!isDeltaSurface)
            {
                hasEncounteredNonDeltaSurface = true;
            }

            const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, surfMipLevel, payload.rng);

            payload.pathWeight *= surfBsdfSample.bsdfValue / surfBsdfSample.pdf;
            if (!surfBsdfSample.wasSpecular)
            {
                payload.pathWeight *= absCosTheta(surfBsdfSample.wi_WS, surfNor_WS);
            }

            if (hitWasWater && dot(surfBsdfSample.wi_WS, surfNor_WS) < 0.f) // apply only for rays that will transmit through the water
            {
                setUnderwaterFromHit(payload, bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT));
            }

            if (pathDepth == 0)
            {
                ptDiffuseAlbedo = payload.pathWeight; // this assumes that emissive surfaces will not scatter (since emissiveContrib is added to ptDiffuseAlbedo earlier)
            }

            setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, surfBsdfSample.wi_WS, true /*faceforwardNormal*/);

            bounceBsdfPdf = surfBsdfSample.pdf;
            bounceWasSpecular = surfBsdfSample.wasSpecular;
        } // !isPassthrough

        ray.TMin = 0.f;
        ray.TMax = RAY_DEFAULT_TMAX;

        payload.flags &= PAYLOAD_FLAG_UNDERWATER; // reset all payload flags except PAYLOAD_FLAG_UNDERWATER
        payload.waterEntryT = RAY_DEFAULT_TMAX;
        payload.waterExitT = RAY_DEFAULT_TMAX;
        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);
        if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const float hitDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            payload.rayCone.width = getRayConeWidthAtDistance(payload.rayCone, hitDistance);

            const Material hitMaterial = getMaterialFromPayload(payload);
            if (hitMaterial.hasDiffuse())
            {
                payload.rayCone.angle += 0.5f;
            }
        }

        const float3 segmentAbsorption = computeSegmentAbsorption(payload, ray.Origin, ray.Direction);
        payload.pathWeight *= segmentAbsorption;

        if (pathDepth == 0)
        {
            // at this point, ptDiffuseAlbedo = first bounce path weight or emission

            if (bounceWasSpecular)
            {
                if (!bool(renderParams.doPathSplitting))
                {
                    ptDiffuseAlbedo = 0.f;
                }
                else
                {
                    bool secondHitHasDiffuseAlbedo = false;
                    float3 secondHitDiffuseAlbedo = 0.f;
                    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
                    {
                        const Material secondHitMaterial = getMaterialFromPayload(payload);
                        if (secondHitMaterial.hasDiffuse())
                        {
                            const float secondHitMipLevel = computeTerrainMipLevel(payload.rayCone.width);
                            const float3 baseColor = getMaterialBaseColor(secondHitMaterial, payload.hitInfo.uv, secondHitMipLevel).rgb;
                            if (any(baseColor > 0.f))
                            {
                                secondHitDiffuseAlbedo = baseColor;
                                secondHitHasDiffuseAlbedo = true;
                            }
                        }
                        if (!secondHitHasDiffuseAlbedo && secondHitMaterial.hasEmission())
                        {
                            const float secondHitMipLevel = computeTerrainMipLevel(payload.rayCone.width);
                            const float3 emissiveColor = getMaterialEmissiveColor(secondHitMaterial, payload.hitInfo.uv, secondHitMipLevel);
                            if (any(emissiveColor > 0.f))
                            {
                                secondHitDiffuseAlbedo = applyReinhard(emissiveColor);
                                secondHitHasDiffuseAlbedo = true;
                            }
                        }
                    }

                    if (secondHitHasDiffuseAlbedo)
                    {
                        ptDiffuseAlbedo *= secondHitDiffuseAlbedo * segmentAbsorption;
                    }
                    else
                    {
                        ptDiffuseAlbedo = 0.f;
                    }
                }
            }

            // if !bounceWasSpecular, ptDiffAlbedo remains unchanged
        }

        if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
        {
            if (doMis)
            {
                const float bsdfSampleDomeLightPdf = domeLightPdf(ray.Direction, surfNor_WS); // 0 if !voxelMode
                const float balanceHeuristicWeight = bounceBsdfPdf / (bounceBsdfPdf + bsdfSampleDomeLightPdf);
                payload.pathWeight *= balanceHeuristicWeight;
            }

            payload.pathColor += payload.pathWeight * getDomeLightColor(ray.Direction);
            return;
        }
        else if (payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            return;
        }

        if (bool(renderParams.doPathSplitting) && pathDepth == 0 && bounceWasSpecular) // TODO: support multiple specular bounces?
        {
            if (pathSplitIdx == 0) // transmission
            {
                RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
                normalsAndRoughnessTarget[pixelIdx].xyz = payload.hitInfo.hitNor_WS;
            }
            else // reflection
            {
                RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
                specularHitDistanceTarget[pixelIdx] = distance(surfPos_WS, payload.hitInfo.hitPos_WS);
            }
        }

        if (doMis)
        {
            // no need to consider dome light pdf here because dome light sampling can't hit area lights

            const Material hitMaterial = getMaterialFromPayload(payload);

            if (hitMaterial.hasEmission() && !bounceWasSpecular)
            {
                const float bsdfSampleLightPdf = lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);

                const float balanceHeuristicWeight = bounceBsdfPdf / (bounceBsdfPdf + bsdfSampleLightPdf);
                payload.pathWeight *= balanceHeuristicWeight;
            }

            // if BSDF sampling hit something other than a light, lightPdf = 0 so misWeight = 1
        }
    }
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = getPixelIdx();
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const uint pathSplitIdx = getPathSplitIdx();

    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    Payload payload;
    payload.hitInfo = gbufferData.hitInfo;
    payload.materialIdx = gbufferData.materialIdx;
    payload.flags = gbufferData.payloadFlags;
    payload.pathWeight = float3(1.f, 1.f, 1.f);
    payload.pathColor = float3(0.f, 0.f, 0.f);
    payload.rng = initRng(constantParams.rngSeed, 987654103, linearPixelIdx * (pathSplitIdx + 1), renderParams.frameNumber);
    payload.waterEntryT = RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone.angle = getRayConePixelAngle();
    payload.rayCone.width = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? payload.rayCone.angle * distance(cameraParams.pos_WS, payload.hitInfo.hitPos_WS)
        : 0.f;

    float3 outPtDiffuseAlbedo;
    pathTraceRay(payload, outPtDiffuseAlbedo);

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

    ptDiffuseAlbedoRawBufferOut[writePixelIdx] = float4(outPtDiffuseAlbedo, 0.f);
}
