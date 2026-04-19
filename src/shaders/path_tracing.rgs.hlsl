// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
#include "radiance_cache.hlsli"
#include "ris.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbufferIn : REGISTER_T(PT, GBUFFER_IN);

#ifdef RC_UPDATE
    #define RC_MAX_PATH_DEPTH 6

    RWByteAddressBuffer rcHashEntries : REGISTER_U(RC, HASH_ENTRIES);
    RWByteAddressBuffer rcAccumulation : REGISTER_U(RC, ACCUMULATION);
#else
    RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);
    RWStructuredBuffer<float4> ptDiffuseAlbedoRawBufferOut : REGISTER_U(PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);

    ByteAddressBuffer rcHashEntries : REGISTER_T(RC, HASH_ENTRIES);
    StructuredBuffer<float4> rcResolved : REGISTER_T(RC, RESOLVED);
#endif

float balanceHeuristic(const float pdfA, const float pdfB)
{
    return pdfA / (pdfA + pdfB);
}

void pathTraceRay(inout Payload payload, out float3 pathColor, out float3 ptDiffuseAlbedo)
{
#ifndef RC_UPDATE
    pathColor = 0.f;
    ptDiffuseAlbedo = 0.f;
#endif

    const uint2 pixelIdx = getPixelIdx();
    const uint pathSplitIdx = getPathSplitIdx();

    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRis = (samplingMode == SamplingMode::RIS);
    const bool doMis = (samplingMode == SamplingMode::MIS || useRis);

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    const float3 segmentAbsorption = computeSegmentAbsorption(payload, cameraParams.pos_WS, ray.Direction);
    payload.pathWeight *= segmentAbsorption;

    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
#ifndef RC_UPDATE
        pathColor = payload.pathWeight * getDomeLightColor(ray.Direction);
#endif
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

#ifdef RC_UPDATE
    uint rcSlots[RC_MAX_PATH_DEPTH];
    float3 rcThroughputs[RC_MAX_PATH_DEPTH];
    uint rcNumVertices = 0;
#endif

    if (sceneParams.voxelMode == 1 && debugParams.colorChunks == 1)
    {
        float3 surfPos_WS = payload.hitInfo.hitPos_WS;
        surfPos_WS.xz += cameraParams.globalInstanceOffset.xz;
        const int2 chunkPosBlocksXZ_WS = int2(floor(surfPos_WS.xz / 16.f)); // should be chunkSizeXZ instead of 16.f but whatever
        const float3 chunkColor = (chunkPosBlocksXZ_WS.x + chunkPosBlocksXZ_WS.y /*z*/) % 2 == 0 ? float3(1.f, 0.5f, 0.5f) : float3(0.5f, 1.f, 1.f);
        payload.pathWeight *= chunkColor;
    }

    Material surfMaterial = getMaterialFromPayload(payload);
#ifdef RC_UPDATE
    const uint effectiveMaxPathDepth = min(renderParams.maxPathDepth, RC_MAX_PATH_DEPTH);
#else
    const uint effectiveMaxPathDepth = renderParams.maxPathDepth;
#endif
    for (uint pathDepth = 0; pathDepth < effectiveMaxPathDepth; ++pathDepth)
    {
        const float surfMipLevel = computeMipLevel(payload.rayCone.width);

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
#ifdef RC_UPDATE
            for (uint i = 0; i < rcNumVertices; ++i)
            {
                if (rcSlots[i] != RC_INVALID_SLOT)
                {
                    rcWriteRadiance(rcSlots[i], rcThroughputs[i] * emissiveContrib, rcAccumulation);
                }
            }
#else
            pathColor += emissiveContrib;

            if (pathDepth == 0)
            {
                ptDiffuseAlbedo += applyReinhard(emissiveContrib);
            }
#endif
        }

        const float3 wo_WS = -ray.Direction;

#ifndef RC_UPDATE
        if (pathDepth == 0 && bool(renderParams.doPathSplitting))
        {
            const bool didSplitMaterial = trySplitMaterial(
                surfMaterial, payload.hitInfo.uv, payload.hitInfo.hitNor_WS, wo_WS, surfMipLevel, pathSplitIdx, payload.pathWeight);
            if (!didSplitMaterial && pathSplitIdx == 1)
            {
                return;
            }
        }
#endif

        const bool isLastBounce = (pathDepth == effectiveMaxPathDepth - 1);
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
            ((!isDeltaSurface && surfMaterial.canScatter()) ? (1 << 0) : 0);
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

            const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, surfMipLevel, payload.rng);

#ifdef RC_UPDATE
            if (!surfBsdfSample.wasSpecular && rcNumVertices < RC_MAX_PATH_DEPTH)
            {
                for (uint i = 0; i < rcNumVertices; ++i)
                {
                    rcThroughputs[i] *= payload.pathWeight;
                }

                const int level = rcGetLevel(surfPos_WS);
                const int3 gridPos = rcWorldToGrid(surfPos_WS, level);
                const uint slot = rcInsertOrFind(gridPos, level, rcHashEntries);
                rcSlots[rcNumVertices] = slot;
                rcThroughputs[rcNumVertices] = float3(1.f, 1.f, 1.f);
                if (slot != RC_INVALID_SLOT)
                {
                    rcWriteSampleCount(slot, rcAccumulation);
                }
                ++rcNumVertices;

                // reset so pathWeight tracks from this vertex onward
                payload.pathWeight = float3(1.f, 1.f, 1.f);
            }
#endif

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

                    float3 contribution =
                        payload.pathWeight * bsdfVal * absCosTheta(lightSample.wi_WS, surfNor_WS) * lightSample.Le;

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

#ifdef RC_UPDATE
                    for (uint i = 0; i < rcNumVertices; ++i)
                    {
                        if (rcSlots[i] != RC_INVALID_SLOT)
                        {
                            rcWriteRadiance(rcSlots[i], rcThroughputs[i] * contribution, rcAccumulation);
                        }
                    }
#else
                    pathColor += contribution;
#endif
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

                        float3 contribution = payload.pathWeight * bsdfVal *
                                              absCosTheta(domeLightSample.wi_WS, surfNor_WS) * domeLightSample.Le;

                        const float domeLightPdf = domeLightSample.pdf;
                        const float domeLightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, domeLightSample.wi_WS, surfNor_WS);
                        const float balanceHeuristicDenominator = domeLightPdf + domeLightSampleBsdfPdf;

                        contribution /= balanceHeuristicDenominator; // dome light pdf in balance heuristic numerator cancels out with divide by pdf

#ifdef RC_UPDATE
                        for (uint i = 0; i < rcNumVertices; ++i)
                        {
                            if (rcSlots[i] != RC_INVALID_SLOT)
                            {
                                rcWriteRadiance(rcSlots[i], rcThroughputs[i] * contribution, rcAccumulation);
                            }
                        }
#else
                        pathColor += contribution;
#endif
                    }
                }
            }

            if (!isDeltaSurface)
            {
                hasEncounteredNonDeltaSurface = true;
            }

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
            surfMaterial = getMaterialFromPayload(payload);

            const float hitDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            payload.rayCone.width = getRayConeWidthAtDistance(payload.rayCone, hitDistance);

            if (surfMaterial.hasDiffuse())
            {
                payload.rayCone.angle += 0.5f;
            }
        }

        const float3 segmentAbsorption = computeSegmentAbsorption(payload, ray.Origin, ray.Direction);
        payload.pathWeight *= segmentAbsorption;

#ifndef RC_UPDATE
        // terminate path early by reading radiance cache if possible
        const bool surfMaterialCanUseRadianceCache = surfMaterial.canScatter() && !surfMaterial.isDelta();
        if (bool(rcParams.rcEnabled) && pathDepth >= 1 && bool(payload.flags & PAYLOAD_FLAG_DID_HIT) &&
            surfMaterialCanUseRadianceCache)
        {
            const float hitDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            const int level = rcGetLevel(payload.hitInfo.hitPos_WS);
            const float voxelSize = rcGetVoxelSize(level);

            if (hitDistance >= voxelSize && payload.rayCone.width >= voxelSize)
            {
                const float3 jitteredPos = rcJitterPos(payload.hitInfo.hitPos_WS, level, payload.rng);
                const int3 gridPos = rcWorldToGrid(jitteredPos, level);
                const uint slot = rcLookup(gridPos, level, rcHashEntries);

                if (slot != RC_INVALID_SLOT)
                {
                    const float4 resolved = rcResolved[slot];
                    if (resolved.w >= rcParams.rcMinSamplesForQuery)
                    {
                        pathColor += payload.pathWeight * resolved.rgb;
                        return;
                    }
                }
            }
        }
#endif

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
                        const float secondHitMipLevel = computeMipLevel(payload.rayCone.width);
                        if (surfMaterial.hasDiffuse())
                        {
                            const float3 baseColor = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, secondHitMipLevel).rgb;
                            if (any(baseColor > 0.f))
                            {
                                secondHitDiffuseAlbedo = baseColor;
                                secondHitHasDiffuseAlbedo = true;
                            }
                        }
                        if (!secondHitHasDiffuseAlbedo && surfMaterial.hasEmission())
                        {
                            const float3 emissiveColor = getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, secondHitMipLevel);
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
            float3 domeLightContrib = payload.pathWeight * getDomeLightColor(ray.Direction);
            if (doMis)
            {
                const float bsdfSampleDomeLightPdf = domeLightPdf(ray.Direction, surfNor_WS); // 0 if !voxelMode
                domeLightContrib *= balanceHeuristic(bounceBsdfPdf, bsdfSampleDomeLightPdf);
            }

#ifdef RC_UPDATE
            for (uint i = 0; i < rcNumVertices; ++i)
            {
                if (rcSlots[i] != RC_INVALID_SLOT)
                {
                    rcWriteRadiance(rcSlots[i], rcThroughputs[i] * domeLightContrib, rcAccumulation);
                }
            }
#else
            pathColor += domeLightContrib;
#endif
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

            if (surfMaterial.hasEmission() && !bounceWasSpecular)
            {
                const float bsdfSampleLightPdf = lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);
                const float emissionMisWeight = balanceHeuristic(bounceBsdfPdf, bsdfSampleLightPdf);
                payload.pathWeight *= emissionMisWeight;
            }

            // if BSDF sampling hit something other than a light, lightPdf = 0 so misWeight = 1
        }
    }
}

[shader("raygeneration")]
void RayGeneration()
{
#ifdef RC_UPDATE
    const uint2 tileIdx = DispatchRaysIndex().xy;
    const uint2 tileOrigin = tileIdx * RC_UPDATE_SCALE;
    const uint2 tileEnd = min(tileOrigin + RC_UPDATE_SCALE, uint2(renderParams.renderSize));
    const uint2 pixelIdx = tileOrigin + uint2(cameraParams.jitter * (tileEnd - tileOrigin));
#else
    const uint2 pixelIdx = getPixelIdx();
    const uint pathSplitIdx = getPathSplitIdx();
#endif

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    Payload payload;
    payload.hitInfo = gbufferData.hitInfo;
    payload.materialIdx = gbufferData.materialIdx;
    payload.flags = gbufferData.payloadFlags;
    payload.pathWeight = float3(1.f, 1.f, 1.f);
#ifdef RC_UPDATE
    payload.rng = initRng(constantParams.rngSeed, 781291012, linearPixelIdx, renderParams.frameNumber);
#else
    payload.rng = initRng(constantParams.rngSeed, 987654103, linearPixelIdx * (pathSplitIdx + 1), renderParams.frameNumber);
#endif
    payload.waterEntryT = RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone.angle = getRayConePixelAngle();
    payload.rayCone.width = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? payload.rayCone.angle * distance(cameraParams.pos_WS, payload.hitInfo.hitPos_WS)
        : 0.f;

    float3 pathColor = 0.f;
    float3 outPtDiffuseAlbedo = 0.f;
    pathTraceRay(payload, pathColor, outPtDiffuseAlbedo);

#ifndef RC_UPDATE
    const uint writePixelIdx = linearPixelIdx * (bool(renderParams.doPathSplitting) ? 2 : 1) + pathSplitIdx;
    if ((AntialiasingMode)renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE && renderParams.accumulatedFrameNumber > 0)
    {
        pathTracingRawBufferOut[writePixelIdx].xyz += pathColor;
    }
    else
    {
        pathTracingRawBufferOut[writePixelIdx].xyz = pathColor;
    }

    ptDiffuseAlbedoRawBufferOut[writePixelIdx] = float4(outPtDiffuseAlbedo, 0.f);
#endif
}
