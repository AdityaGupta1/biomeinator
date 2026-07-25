// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#define HITGROUP_LIGHTS PT_HITGROUP_LIGHTS

#include "common/nvapi_includes.hlsli"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "light/dome_light.hlsli"
#include "light/fog.hlsli"
#include "light/light_sampling.hlsli"
#include "light/ris.hlsli"
#include "common/light_tree_sampling.hlsli"
#include "materials/materials.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#if NRC_UPDATE || NRC_QUERY
    #define NRC_USE_CUSTOM_BUFFER_ACCESSORS 1
    #define NRC_BUFFER_QUERY_PATH_INFO nrcQueryPathInfo
    #define NRC_BUFFER_TRAINING_PATH_INFO nrcTrainingPathInfo
    #define NRC_BUFFER_TRAINING_PATH_VERTICES nrcTrainingPathVertices
    #define NRC_BUFFER_QUERY_RADIANCE_PARAMS nrcQueryRadianceParams
    #define NRC_BUFFER_QUERY_COUNTERS_DATA nrcCountersData

    #include "NrcStructures.h"

    cbuffer NrcConstantBuffer : REGISTER_B(NRC, NRC_CONSTANTS)
    {
        NrcConstants nrcConstants;
    };

    RWStructuredBuffer<NrcPackedQueryPathInfo> nrcQueryPathInfo : REGISTER_U(NRC, QUERY_PATH_INFO);
    RWStructuredBuffer<NrcPackedTrainingPathInfo> nrcTrainingPathInfo : REGISTER_U(NRC, TRAINING_PATH_INFO);
    RWStructuredBuffer<NrcPackedPathVertex> nrcTrainingPathVertices : REGISTER_U(NRC, TRAINING_PATH_VERTICES);
    RWStructuredBuffer<NrcRadianceParams> nrcQueryRadianceParams : REGISTER_U(NRC, QUERY_RADIANCE_PARAMS);
    RWStructuredBuffer<uint> nrcCountersData : REGISTER_U(NRC, COUNTERS_DATA);

    #include "Nrc.hlsli"
#endif

StructuredBuffer<GbufferData> gbufferIn : REGISTER_T(PT, GBUFFER_IN);

#if !NRC_UPDATE
    RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);
    RWStructuredBuffer<float4> ptDiffuseAlbedoRawBufferOut : REGISTER_U(PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);
#endif

// Detects hitting a water backface without having crossed a water front face or started
// underwater — happens when partially loaded chunks leave water volumes open. Paths are
// terminated at such hits: continuing would trace the open water interior flagged as air
// (fog in-scatter below sea level, unattenuated dome light) which glows and flickers.
bool isOrphanWaterBackfaceHit(const Payload payload)
{
    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT) || !bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT) ||
        bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        return false;
    }

    if (payload.waterEntryT != RAY_DEFAULT_TMAX)
    {
        return false;
    }

    const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
    return bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
}

void pathTraceRay(inout Payload payload, const uint2 pixelIdx, const uint pathSplitIdx, out float3 pathColor, out float3 ptDiffuseAlbedo)
{
    pathColor = 0.f;
    ptDiffuseAlbedo = 0.f;

#if NRC_UPDATE || NRC_QUERY
    // NRC frameDimensions match full dispatch (e.g. doubled width when path splitting).
    // Do not use getPixelIdx() here - it collapses split lanes to the same coordinate.
    const uint2 nrcPixelIdx = DispatchRaysIndex().xy;
    NrcBuffers nrcBufs = (NrcBuffers)0;
    NrcContext nrcCtx = NrcCreateContext(nrcConstants, nrcBufs, nrcPixelIdx);
    NrcPathState nrcPathState = NrcCreatePathState(nrcConstants, payload.rng.nextFloat());
#endif

    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRis = (samplingMode == SamplingMode::RIS);
    const bool useRtsl = (samplingMode == SamplingMode::RTSL);
    const bool doMis = (samplingMode == SamplingMode::MIS || useRis || useRtsl);

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    const float3 segmentAbsorption = computeSegmentAbsorption(payload, cameraParams.pos_WS, ray.Direction);

    const bool fogEnabled = sceneParams.voxelMode == 1 && renderParams.fogSigmaS > 0.f;
    if (fogEnabled && !bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        const float segmentDist = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
            ? distance(cameraParams.pos_WS, payload.hitInfo.hitPos_WS)
            : getDistanceToVoxelBounds(cameraParams.pos_WS, ray.Direction);
        // The primary segment is identical for both path splits and collect sums them, so
        // in-scattered radiance is added only by split 0 (same as emission and the dome light miss).
        float fogTransmittance;
        if (pathSplitIdx == 0)
        {
            const float3 inScatter = computeFogInScatter(
                cameraParams.pos_WS, ray.Direction, segmentDist, renderParams.fogMarchSteps, payload.rng, fogTransmittance);
            pathColor += payload.pathWeight * inScatter;
        }
        else
        {
            fogTransmittance = computeFogTransmittance(cameraParams.pos_WS, ray.Direction, segmentDist);
        }
        payload.pathWeight *= fogTransmittance;
    }

    payload.pathWeight *= segmentAbsorption;

    if (isOrphanWaterBackfaceHit(payload))
    {
#if NRC_UPDATE || NRC_QUERY
        NrcUpdateOnMiss(nrcPathState);
        NrcWriteFinalPathInfo(nrcCtx, nrcPathState, payload.pathWeight, pathColor);
#endif
        return;
    }

    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        const float3 domeLightColor = (pathSplitIdx == 0) ? getDomeLightColor(ray.Direction) : 0.f;
#if NRC_UPDATE || NRC_QUERY
        NrcUpdateOnMiss(nrcPathState);
        NrcWriteFinalPathInfo(nrcCtx, nrcPathState, payload.pathWeight, domeLightColor);
#endif
        pathColor += payload.pathWeight * domeLightColor;
        if (sceneParams.voxelMode == 1)
        {
            // Give the sky an albedo so DLSS doesn't see it as black. Uses the unattenuated dome
            // light rather than pathWeight, which would fold in fog transmittance.
            ptDiffuseAlbedo = applyReinhard(domeLightColor);
        }
        return;
    }

    if (payload.materialIdx == MATERIAL_IDX_INVALID)
    {
#if NRC_UPDATE || NRC_QUERY
        NrcUpdateOnMiss(nrcPathState);
        NrcWriteFinalPathInfo(nrcCtx, nrcPathState, payload.pathWeight, pathColor);
#endif
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

    Material surfMaterial = getMaterialFromPayload(payload);
    const uint effectiveMaxPathDepth = renderParams.maxPathDepth;
    for (uint pathDepth = 0; pathDepth < effectiveMaxPathDepth; ++pathDepth)
    {
        const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
        const bool hitWasWater = bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
        TexSampleCtx surfTexCtx;
        surfTexCtx.mipLevel = computeMipLevel(payload.rayCone.width);
        surfTexCtx.arraySliceIdx = perTriData.texArraySliceIdx;

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and multiply by Fresnel reflectance
        // In RIS mode, only include emission if this is the first bounce (pathDepth == 0) or the previous event was a delta event (specular)
        float3 emissiveContrib = 0.f;
        if ((pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
        {
            emissiveContrib = payload.pathWeight * getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, surfTexCtx);
        }

        const float3 wo_WS = -ray.Direction;

#if !NRC_UPDATE
        if (pathDepth == 0 && bool(renderParams.doPathSplitting))
        {
            const bool didSplitMaterial = trySplitMaterial(
                surfMaterial, payload.hitInfo.uv, payload.hitInfo.hitNor_WS, wo_WS, surfTexCtx, pathSplitIdx, payload.pathWeight);
            if (!didSplitMaterial && pathSplitIdx == 1)
            {
                break;
            }
        }
#endif

#if NRC_UPDATE || NRC_QUERY
        NrcSurfaceAttributes nrcSurfAttr;
        nrcSurfAttr.encodedPosition = NrcEncodePosition(payload.hitInfo.hitPos_WS, nrcConstants);
        nrcSurfAttr.roughness = surfMaterial.isDelta() ? 0.0f : 1.0f;
        nrcSurfAttr.specularF0 = surfMaterial.hasGlossyReflection()
            ? surfMaterial.glossyReflectionTint : float3(0, 0, 0);
        nrcSurfAttr.diffuseReflectance = surfMaterial.hasDiffuse()
            ? getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb
            : float3(0, 0, 0);
        nrcSurfAttr.shadingNormal = payload.hitInfo.hitNor_WS;
        nrcSurfAttr.viewVector = wo_WS;
        nrcSurfAttr.isDeltaLobe = surfMaterial.isDelta();

        const float nrcHitDist = distance(
            (pathDepth == 0) ? cameraParams.pos_WS : ray.Origin,
            payload.hitInfo.hitPos_WS);

        const NrcProgressState nrcProgress = NrcUpdateOnHit(
            nrcCtx, nrcPathState, nrcSurfAttr, nrcHitDist, pathDepth,
            payload.pathWeight, pathColor);

        if (nrcProgress == NrcProgressState::TerminateImmediately)
        {
            break;
        }
#endif

        // In voxel mode all terrain shares one material with hasDiffuse=true; emissive blocks
        // like LAMP/LAVA have zero diffuse in the texture, so skip scatter work in that case
        // to avoid pointless shadow rays and BSDF sampling.
        bool isPureEmitter = false;
        if (any(emissiveContrib > 0))
        {
            pathColor += emissiveContrib;
            if (pathDepth == 0)
            {
                ptDiffuseAlbedo += applyReinhard(emissiveContrib);
            }

            const bool isDiffuseOnly = surfMaterial.hasDiffuse()
                && !surfMaterial.hasGlossyReflection()
                && !surfMaterial.hasGlossyTransmission();
            if (isDiffuseOnly)
            {
                const float3 baseColor = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb;
                isPureEmitter = !any(baseColor > 0.f);
            }
        }

        const bool isLastBounce = (pathDepth == effectiveMaxPathDepth - 1);
        if (!surfMaterial.canScatter() || isLastBounce || isPureEmitter)
        {
            break;
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
            payload.pathWeight *= getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb;
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
                bool rrShouldTerminate = false;
#if NRC_UPDATE || NRC_QUERY
                if (NrcCanUseRussianRoulette(nrcPathState))
#endif
                {
                    const float survivalProbability = max(saturate(luminance(payload.pathWeight)), 0.1f);
                    if (payload.rng.nextFloat() >= survivalProbability)
                    {
                        rrShouldTerminate = true;
                    }
                    else
                    {
                        payload.pathWeight /= survivalProbability;
                    }
                }
                if (rrShouldTerminate)
                {
                    break;
                }
            }

            const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, surfTexCtx, payload.rng);

#if NRC_UPDATE || NRC_QUERY
            NrcSetBrdfPdf(nrcPathState, surfBsdfSample.pdf);
            // NRC updates this flag before BSDF sampling, but mixed materials only reveal
            // whether the outgoing lobe is delta after sampling.
            NrcSetFlag(nrcPathState.packedData, nrcPathFlagPreviousHitWasDeltaLobe, surfBsdfSample.wasSpecular);
#endif

            if (doMis && surfMaterial.canScatter() && !isDeltaSurface)
            {
                // ------------------------------
                // sample area lights
                // ------------------------------

                const bool isUnderwater = payload.flags & PAYLOAD_FLAG_UNDERWATER;

                DirectLightingSample lightSample;
                if (useRtsl)
                {
                    lightSample = sampleDirectLightingRtsl(
                        surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater, payload.rng);
                }
                else if (useRis)
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
                        surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS, surfTexCtx);

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

                        contribution *= W * balanceHeuristic(lightPdf, lightSampleBsdfPdf);
                    }
                    else
                    {
                        const float lightPdf = lightSample.pdfOrW_Y;
                        const float balanceHeuristicDenominator = lightPdf + lightSampleBsdfPdf;

                        contribution /= balanceHeuristicDenominator; // light pdf in balance heuristic numerator cancels out with divide by pdf
                    }

                    pathColor += contribution;
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

                        const float3 bsdfVal = evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, domeLightSample.wi_WS, surfNor_WS, surfTexCtx);

                        float3 contribution = payload.pathWeight * bsdfVal *
                                              absCosTheta(domeLightSample.wi_WS, surfNor_WS) * domeLightSample.Le;

                        const float domeLightPdf = domeLightSample.pdf;
                        const float domeLightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, domeLightSample.wi_WS, surfNor_WS);
                        const float balanceHeuristicDenominator = domeLightPdf + domeLightSampleBsdfPdf;

                        contribution /= balanceHeuristicDenominator; // dome light pdf in balance heuristic numerator cancels out with divide by pdf

                        pathColor += contribution;
                    }
                }
            }

            if (!isDeltaSurface)
            {
                hasEncounteredNonDeltaSurface = true;
            }

#if NRC_UPDATE || NRC_QUERY
            if (nrcProgress == NrcProgressState::TerminateAfterDirectLighting)
            {
                break;
            }
#endif

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

        float fogTransmittance = 1.f;
        if (fogEnabled && !bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
        {
            const float segmentDist = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
                ? distance(ray.Origin, payload.hitInfo.hitPos_WS)
                : getDistanceToVoxelBounds(ray.Origin, ray.Direction);
            // Restrict in-scattering to early path depths; deeper bounces keep only
            // transmittance. Bounce segments diverge after the path split, so no split
            // gating here — this puts god rays into the reflection split.
            const uint numFogSteps = (pathDepth <= 1) ? max(renderParams.fogMarchSteps / 2, 1u) : 0u;
            const float3 inScatter =
                computeFogInScatter(ray.Origin, ray.Direction, segmentDist, numFogSteps, payload.rng, fogTransmittance);
            pathColor += payload.pathWeight * inScatter;
            payload.pathWeight *= fogTransmittance;
        }

        payload.pathWeight *= segmentAbsorption;

        if (isOrphanWaterBackfaceHit(payload))
        {
            break;
        }

        const bool didMiss = !bool(payload.flags & PAYLOAD_FLAG_DID_HIT);
        const float3 missDomeLightColor = didMiss ? getDomeLightColor(ray.Direction) : float3(0.f, 0.f, 0.f);

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
                        TexSampleCtx secondHitTexCtx;
                        secondHitTexCtx.mipLevel = computeMipLevel(payload.rayCone.width);
                        secondHitTexCtx.arraySliceIdx = perTriDatas[instanceDatas[payload.hitInfo.instanceId].perTriDatasBufferOffset + payload.hitInfo.triangleIdx].texArraySliceIdx;
                        if (surfMaterial.hasDiffuse())
                        {
                            const float3 baseColor = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, secondHitTexCtx).rgb;
                            if (any(baseColor > 0.f))
                            {
                                secondHitDiffuseAlbedo = baseColor;
                                secondHitHasDiffuseAlbedo = true;
                            }
                        }
                        if (!secondHitHasDiffuseAlbedo && surfMaterial.hasEmission())
                        {
                            const float3 emissiveColor = getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, secondHitTexCtx);
                            if (any(emissiveColor > 0.f))
                            {
                                secondHitDiffuseAlbedo = applyReinhard(emissiveColor);
                                secondHitHasDiffuseAlbedo = true;
                            }
                        }
                    }

                    if (secondHitHasDiffuseAlbedo)
                    {
                        ptDiffuseAlbedo *= secondHitDiffuseAlbedo * segmentAbsorption * fogTransmittance;
                    }
                    else if (didMiss && sceneParams.voxelMode == 1)
                    {
                        // Specular reflection of the sky. Excludes fog and absorption to match
                        // how the primary miss builds its albedo.
                        ptDiffuseAlbedo *= applyReinhard(missDomeLightColor);
                    }
                    else
                    {
                        ptDiffuseAlbedo = 0.f;
                    }
                }
            }

            // if !bounceWasSpecular, ptDiffAlbedo remains unchanged
        }

        if (didMiss)
        {
            float3 domeLightContrib = payload.pathWeight * missDomeLightColor;
            if (doMis)
            {
                const float bsdfSampleDomeLightPdf = domeLightPdf(ray.Direction, surfNor_WS); // 0 if !voxelMode
                domeLightContrib *= balanceHeuristic(bounceBsdfPdf, bsdfSampleDomeLightPdf);
            }

#if NRC_UPDATE || NRC_QUERY
            NrcUpdateOnMiss(nrcPathState);
#endif
            pathColor += domeLightContrib;
            break;
        }
        else if (payload.materialIdx == MATERIAL_IDX_INVALID)
        {
#if NRC_UPDATE || NRC_QUERY
            NrcUpdateOnMiss(nrcPathState);
#endif
            break;
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
                const float bsdfSampleLightPdf = useRtsl
                    ? lightPdfRtsl(payload.hitInfo, surfPos_WS, surfNor_WS, ray.Direction)
                    : lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);
                const float emissionMisWeight = balanceHeuristic(bounceBsdfPdf, bsdfSampleLightPdf);
                payload.pathWeight *= emissionMisWeight;
            }

            // if BSDF sampling hit something other than a light, lightPdf = 0 so misWeight = 1
        }
    }

#if NRC_UPDATE || NRC_QUERY
    NrcWriteFinalPathInfo(nrcCtx, nrcPathState, payload.pathWeight, pathColor);
#endif
}

[shader("raygeneration")]
void RayGeneration()
{
#if NRC_UPDATE
    const uint2 trainingPixelIdx = DispatchRaysIndex().xy;
    const uint2 pixelIdx = min(
        uint2(trainingPixelIdx * uint2(renderParams.renderSize) / nrcConstants.trainingDimensions),
        uint2(renderParams.renderSize) - 1);
    const uint pathSplitIdx = 0;
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
#if NRC_UPDATE
    payload.rng = initRng(constantParams.rngSeed, 391827465, linearPixelIdx, renderParams.frameNumber);
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
    pathTraceRay(payload, pixelIdx, pathSplitIdx, pathColor, outPtDiffuseAlbedo);

#if !NRC_UPDATE
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
