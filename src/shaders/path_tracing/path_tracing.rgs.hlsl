// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/nvapi_includes.hlsli"

#include "common/global_params.hlsli"
#include "common/light_tree_sampling.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "light/dome_light.hlsli"
#include "light/fog.hlsli"
#include "light/light_sampling.hlsli"
#include "materials/materials.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#ifndef SHARC_DECOMPOSE
#define SHARC_DECOMPOSE 0
#endif
#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif
#if SHARC_UPDATE || SHARC_QUERY
#include "sharc/sharc_common.hlsli"
#endif

StructuredBuffer<GbufferData> gbufferIn : REGISTER_T(PT, GBUFFER_IN);

RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);
RWStructuredBuffer<float4> ptDiffuseAlbedoRawBufferOut : REGISTER_U(PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);

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

// Adds the segment's fog in-scatter to pathColor and folds fog transmittance into
// pathWeight. Returns the segment's fog transmittance (1 if fog is inactive for this segment).
float applySegmentFog(inout Payload payload, const float3 origin_WS, const float3 dir,
    const uint numInScatterSteps, inout float3 pathColor)
{
    const bool fogEnabled = sceneParams.voxelMode == 1 && renderParams.fogSigmaS > 0.f;
    if (!fogEnabled || bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        return 1.f;
    }

    const float segmentDist = getSegmentVolumeDistance(payload, origin_WS, dir);

    float fogTransmittance;
    const float3 inScatter =
        computeFogInScatter(origin_WS, dir, segmentDist, numInScatterSteps, payload.rng, fogTransmittance);
    pathColor += payload.pathWeight * inScatter;
    payload.pathWeight *= fogTransmittance;
    return fogTransmittance;
}

// The two guide buffers for a first bounce whose lobes are picked stochastically. Taking the guide from
// the path weight (as the delta path does) would feed the denoiser the per-sample lobe choice as noise,
// so the lobes are weighted analytically with the macro-normal Fresnel instead. This is the same
// weighting evaluateBsdf gives a rough glossy reflection over diffuse; rough glass weights its lobes per
// microfacet when sampling, but the macro-normal split is good enough for a guide buffer.
struct FirstBounceAlbedos
{
    float3 diffuse;
    float3 specular;
};

// weight is the path weight before scattering, so both guides inherit everything the path accumulated up
// to the first bounce (volume absorption, fog transmittance, the alpha split weight) and stay consistent
// with the radiance they demodulate.
FirstBounceAlbedos computeFirstBounceAlbedos(const Material material,
                                             const float2 uv,
                                             const float3 wo_WS,
                                             const float3 surfNor_WS,
                                             const TexSampleCtx texCtx,
                                             const float3 weight)
{
    const float fresnelReflectance = glossyReflectionProbability(material, wo_WS, surfNor_WS);
    const float3 glossyReflectionAlbedo = calculateDlssSpecularAlbedo(
        material.glossyReflectionTint, material.roughness * material.roughness, cosTheta(wo_WS, surfNor_WS));
    // The lobe the light reaches when it isn't reflected: diffuse, or transmission for glass
    const float3 nonReflectedAlbedo =
        weight * (1.f - fresnelReflectance) * getMaterialBaseColor(material, uv, texCtx).rgb;
    // Refraction through rough glass reads as a specular signal, and glass has no diffuse lobe to put it in
    const bool nonReflectedIsSpecular = material.hasGlossyTransmission();

    FirstBounceAlbedos result;
    result.diffuse = nonReflectedIsSpecular ? float3(0.f, 0.f, 0.f) : nonReflectedAlbedo;
    result.specular = weight * fresnelReflectance * glossyReflectionAlbedo;
    if (nonReflectedIsSpecular)
    {
        result.specular += nonReflectedAlbedo;
    }
    return result;
}

struct PathRadianceBreakdown
{
    float3 primaryNee;
    float3 cached;
    float3 firstRayEmission;
    float3 laterNee;
    float3 laterEmission;
    float3 visibleEmission;
};

void pathTraceRay(inout Payload payload, const uint2 pixelIdx, const uint pathSplitIdx,
    out float3 pathColor, out float3 ptDiffuseAlbedo, out PathRadianceBreakdown breakdown)
{
    pathColor = 0.f;
    ptDiffuseAlbedo = 0.f;
    breakdown = (PathRadianceBreakdown)0;

    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRtsl = (samplingMode == SamplingMode::RTSL);
    const bool doMis = (samplingMode == SamplingMode::MIS || useRtsl);

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    const float3 segmentAbsorption = computeSegmentAbsorption(payload, cameraParams.pos_WS, ray.Direction);

    // The primary segment is identical for both path splits and collect sums them, so
    // in-scattered radiance is added only by split 0 (same as emission and the dome light miss).
    applySegmentFog(payload, cameraParams.pos_WS, ray.Direction,
        (pathSplitIdx == 0) ? renderParams.fogMarchSteps : 0u, pathColor);

    payload.pathWeight *= segmentAbsorption;

    if (isOrphanWaterBackfaceHit(payload))
    {
        return;
    }

    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        const float3 domeLightColor = (pathSplitIdx == 0) ? getDomeLightColor(ray.Direction) : 0.f;
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
        return;
    }

#if SHARC_UPDATE
    SharcState sharcState;
    SharcInit(sharcState);
    // Primary-segment radiance and attenuation belong to the camera, not this surface.
    pathColor = 0.f;
    payload.pathWeight = 1.f;
#endif
#if SHARC_QUERY
    bool cacheHit = false;
    uint tracedBounces = 0;
#endif

    // data of last "real" bounce (i.e. not passthrough)
    bool bounceWasSpecular = false; // TODO: pack this and bounceAcceptedBacksideLight together (and see if they can be eliminated entirely)
    bool bounceAcceptedBacksideLight = false;
    float bounceBsdfPdf = 0.f;
    float3 surfPos_WS, surfNor_WS;

    bool hasEncounteredNonDeltaSurface = false;

    // Emission seen at the primary hit, kept apart from the scattered part of the albedo guide so the
    // specular look-through below can modulate the scattered part alone; folded in after the loop.
    float3 ptEmissiveAlbedo = 0.f;

    if (sceneParams.voxelMode == 1 && debugParams.colorChunks == 1)
    {
        float3 surfPos_WS = payload.hitInfo.hitPos_WS;
        surfPos_WS.xz += cameraParams.globalInstanceOffset.xz;
        const int2 chunkPosBlocksXZ_WS = int2(floor(surfPos_WS.xz / 16.f)); // should be chunkSizeXZ instead of 16.f but whatever
        const float3 chunkColor = (chunkPosBlocksXZ_WS.x + chunkPosBlocksXZ_WS.y /*z*/) % 2 == 0 ? float3(1.f, 0.5f, 0.5f) : float3(0.5f, 1.f, 1.f);
        payload.pathWeight *= chunkColor;
    }

    Material surfMaterial = getHitMaterial(payload, payload.rayCone.width);
#if SHARC_QUERY
    // Cohort follows the resolved primary material, regardless of later BSDF choices.
    const bool primaryGlass = surfMaterial.hasGlossyTransmission();
    bool primaryGlassQueried = false;
    if (primaryGlass)
        sharcCount(8);
#endif
    const uint effectiveMaxPathDepth = renderParams.maxPathDepth;
    for (uint pathDepth = 0; pathDepth < effectiveMaxPathDepth; ++pathDepth)
    {
#if SHARC_UPDATE
        // Fold the preceding segment's BSDF, passthrough tint and volume attenuation
        // into all stored vertices before starting an independent local estimate.
        SharcSetThroughput(sharcState, payload.pathWeight);
        payload.pathWeight = 1.f;
        pathColor = 0.f;
        if (!surfMaterial.isDelta())
            surfMaterial.roughness = max(surfMaterial.roughness, sharcParams.roughnessMin);
#endif
        const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
        const bool hitWasWater = bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
        const TexSampleCtx surfTexCtx =
            makeTintedTexSampleCtx(perTriData, payload.rayCone.width, payload.hitInfo.hitPos_WS);

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and
        // multiply by Fresnel reflectance
        float3 emissiveContrib = 0.f;
        if ((pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
        {
            const float3 hitEmission = getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, surfTexCtx);
#if SHARC_QUERY
            // Count actual emissive surface hits independently of their throughput/MIS weight.
            if (pathDepth == 1 && any(hitEmission > 0.f))
                sharcCount(7);
#endif
            emissiveContrib = payload.pathWeight * hitEmission;

            // MIS against direct light sampling from the previous real vertex. Only the emission term is weighted:
            // a path continuing past this surface can only come from BSDF sampling (NEE terminates at the light),
            // so the continuation keeps its full throughput.
            // No need to consider dome light pdf here because dome light sampling can't hit area lights.
            if (pathDepth > 0 && doMis && !bounceWasSpecular)
            {
                const float bsdfSampleLightPdf = useRtsl
                    ? lightPdfRtsl(payload.hitInfo, surfPos_WS, surfNor_WS, ray.Direction, bounceAcceptedBacksideLight)
                    : lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);
                emissiveContrib *= balanceHeuristic(bounceBsdfPdf, bsdfSampleLightPdf);
            }
        }

        const float3 wo_WS = -ray.Direction;

        if (!SHARC_UPDATE && pathDepth == 0 && bool(renderParams.doPathSplitting))
        {
            const bool didSplitMaterial = trySplitMaterial(
                surfMaterial, payload.hitInfo.uv, payload.hitInfo.hitNor_WS, wo_WS, surfTexCtx, pathSplitIdx, payload.pathWeight);
            if (!didSplitMaterial && pathSplitIdx == 1)
            {
                break;
            }
        }

        // Resolve the sampled base color once per bounce so downstream albedo and BSDF reads take
        // the constant-color path instead of re-sampling the base and aux textures every call.
        surfMaterial.baseColor = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb;
        surfMaterial.baseColorTextureId = TEXTURE_ID_INVALID;

        // In voxel mode all terrain shares one material with hasDiffuse=true; emissive blocks
        // like LAMP/LAVA have zero diffuse in the texture, so skip scatter work in that case
        // to avoid pointless shadow rays and BSDF sampling.
        bool isPureEmitter = false;
        if (any(emissiveContrib > 0))
        {
            pathColor += emissiveContrib;
#if SHARC_DECOMPOSE
            if (pathDepth == 1)
                breakdown.firstRayEmission += emissiveContrib;
            else if (pathDepth > 1)
                breakdown.laterEmission += emissiveContrib;
            else
                breakdown.visibleEmission += emissiveContrib;
#endif
            if (pathDepth == 0)
            {
                ptEmissiveAlbedo = applyReinhard(emissiveContrib);
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
#if SHARC_UPDATE
            SharcUpdateMiss(makeSharcParameters(), sharcState, pathColor);
#endif
            break;
        }

        const bool isDeltaSurface = surfMaterial.isDelta();

        // canPassthrough = has the path encountered a non-delta surface (including this one)
        // isPassthrough = this intersection has glossy transmission and should be passed through
        const bool canPassthrough = bool(renderParams.refractionIndirectPassthrough) && (!isDeltaSurface || hasEncounteredNonDeltaSurface);
        const bool isPassthrough = canPassthrough && surfMaterial.isDeltaTransmission();

        // If this is a passthrough "bounce", we don't care about its hit pos/nor and want to instead preserve the last
        // "real" bounce's information. This is important for matching MIS weights with direct light sampling, which
        // traces only one ray and ignores passthrough surfaces in the anyhit shader.
        if (!isPassthrough)
        {
            surfNor_WS = payload.hitInfo.hitNor_WS;
            surfPos_WS = payload.hitInfo.hitPos_WS;
        }

#if SHARC_QUERY
        const bool diffuseCacheSurface =
            surfMaterial.hasDiffuse() && !surfMaterial.hasGlossyReflection() && !surfMaterial.hasGlossyTransmission();
        if (pathDepth > 0 && diffuseCacheSurface && !isPassthrough)
        {
            const SharcParameters cache = makeSharcParameters();
            const SharcHitData hit =
                makeSharcHit(payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS, surfMaterial.baseColor);
            const uint level = HashGridGetLevel(hit.positionWorld, cache.hashGridParameters);
            const float segmentLength = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            const float voxelSize = HashGridGetVoxelSize(level, cache.hashGridParameters);
            // Width is the incoming cone diameter, before scattering at this vertex.
            // Two cells preserves the sample's conservative radius-vs-cell threshold.
            if (segmentLength > sqrt(3.f) * voxelSize && payload.rayCone.width > 2.f * voxelSize)
            {
                sharcCount(0);
                if (primaryGlass && !primaryGlassQueried)
                {
                    sharcCount(11); // Unique paths reaching an eligible lookup, not lookup attempts.
                    primaryGlassQueried = true;
                }
                float3 radiance;
                if (SharcGetCachedRadiance(cache, hit, radiance, false))
                {
                    // Emission has already been added using the previous vertex's MIS weight.
                    // Query returns scattered radiance only; skip this vertex's NEE and BSDF.
                    const float3 cachedContribution = payload.pathWeight * radiance;
                    pathColor += cachedContribution;
#if SHARC_DECOMPOSE
                    breakdown.cached += cachedContribution;
#endif
                    sharcCount(1);
                    if (primaryGlass)
                        sharcCount(10);
                    cacheHit = true;
                    break;
                }
            }
        }
#endif
        const uint coherenceHint = (pathDepth == 0 ? (1 << 2) : 0) | (isPassthrough ? (1 << 1) : 0) |
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
            // bounceBsdfPdf, bounceWasSpecular, etc. are intentionally preserved from the last real BSDF sample
        }
        else // !isPassthrough
        {
#if SHARC_UPDATE
            // Keep local NEE separate from emission, including on very bright emitters.
            pathColor = 0.f;
#endif
            // russian roulette
            if (!SHARC_UPDATE && pathDepth >= 2)
            {
                const float survivalProbability = max(saturate(luminance(payload.pathWeight)), 0.1f);
                if (nextFloat(payload.rng) >= survivalProbability)
                {
                    break;
                }
                payload.pathWeight /= survivalProbability;
            }

            const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, surfTexCtx, payload.rng);

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
                        surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater,
                        surfMaterial.acceptsBacksideLight(), payload.rng);
                }
                else
                {
                    lightSample =
                        sampleDirectLightingUniform(surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater, payload.rng);
                }

                if (lightSample.didHitLight)
                {
                    // no need to consider dome light pdf because dome light sampling can't hit area lights

                    const BsdfEval bsdfEval =
                        evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS, surfTexCtx);

                    float3 contribution =
                        payload.pathWeight * bsdfEval.value * absCosTheta(lightSample.wi_WS, surfNor_WS) * lightSample.Le;

                    const float balanceHeuristicDenominator = lightSample.pdf + bsdfEval.pdf;

                    contribution /= balanceHeuristicDenominator; // light pdf in balance heuristic numerator cancels out
                                                                 // with divide by pdf

                    pathColor += contribution;
#if SHARC_DECOMPOSE
                    if (pathDepth == 0)
                        breakdown.primaryNee += contribution;
                    else
                        breakdown.laterNee += contribution;
#endif
                }

                // ------------------------------
                // sample dome light
                // ------------------------------

                if (sceneParams.voxelMode == 1)
                {
                    DomeLightSample domeLightSample = sampleDomeLight(surfPos_WS, surfNor_WS, payload.rayCone,
                        canPassthrough, isUnderwater, surfMaterial.acceptsBacksideLight(), payload.rng);
                    if (domeLightSample.didReachDomeLight)
                    {
                        // no need to consider area light pdf because area light sampling can't hit dome light

                        const BsdfEval bsdfEval = evaluateBsdf(
                            surfMaterial, payload.hitInfo.uv, wo_WS, domeLightSample.wi_WS, surfNor_WS, surfTexCtx);

                        float3 contribution = payload.pathWeight * bsdfEval.value *
                                              absCosTheta(domeLightSample.wi_WS, surfNor_WS) * domeLightSample.Le;

                        const float balanceHeuristicDenominator = domeLightSample.pdf + bsdfEval.pdf;

                        contribution /= balanceHeuristicDenominator; // dome light pdf in balance heuristic numerator
                                                                     // cancels out with divide by pdf

                        pathColor += contribution;
#if SHARC_DECOMPOSE
                    if (pathDepth == 0)
                        breakdown.primaryNee += contribution;
                    else
                        breakdown.laterNee += contribution;
#endif
                    }
                }
            }

#if SHARC_UPDATE
            const bool diffuseCacheSurface = surfMaterial.hasDiffuse() && !surfMaterial.hasGlossyReflection() &&
                                             !surfMaterial.hasGlossyTransmission();
            if (diffuseCacheSurface)
            {
                SharcHitData hit = makeSharcHit(surfPos_WS, surfNor_WS, surfMaterial.baseColor);
                hit.emissive = emissiveContrib;
                sharcCount(3);
                if (!SharcUpdateHit(makeSharcParameters(), sharcState, hit, pathColor, nextFloat(payload.rng)))
                {
                    // False can mean successful cache resampling or hash allocation failure.
                    HashGridKey key;
                    if (sharcParams.diagnostics && HashGridFindEntry(makeSharcParameters().hashGridData,
                                                                     hit.positionWorld,
                                                                     hit.normalWorld,
                                                                     makeSharcParameters().hashGridParameters,
                                                                     key) == HASH_GRID_INVALID_CACHE_INDEX)
                        sharcCount(4);
                    break;
                }
            }
            else
            {
                // Specular vertices carry lighting to earlier diffuse entries but never store
                // a direction-dependent outgoing value in the diffuse cache.
                SharcUpdateMiss(makeSharcParameters(), sharcState, pathColor + emissiveContrib);
            }
            pathColor = 0.f;
#endif

            if (!isDeltaSurface)
            {
                hasEncounteredNonDeltaSurface = true;
            }

            // A rough glossy first bounce would otherwise write the stochastically chosen lobe's weight as its
            // albedo. Only pathSplitIdx 0 can reach a rough material (trySplitMaterial breaks split 1 out for
            // anything it can't split, and the alpha split makes split 1 a delta passthrough), so the single
            // write to the shared specular albedo target has no other writer to race with.
            const bool useAnalyticAlbedoGuides =
                (pathDepth == 0) && surfMaterial.hasGlossy() && surfMaterial.roughness > 0.f;
            if (useAnalyticAlbedoGuides)
            {
                const FirstBounceAlbedos albedos = computeFirstBounceAlbedos(
                    surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, surfTexCtx, payload.pathWeight);
                ptDiffuseAlbedo = albedos.diffuse;

                RWTexture2D<float4> specularAlbedoTarget =
                    ResourceDescriptorHeap[heapIndices.uav.specularAlbedoTargetIdx];
                if (!SHARC_UPDATE)
                    specularAlbedoTarget[pixelIdx] = float4(albedos.specular, 1.f);
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

            if (pathDepth == 0 && !useAnalyticAlbedoGuides)
            {
                ptDiffuseAlbedo = payload.pathWeight;
            }

            if (all(payload.pathWeight == 0.f)) // dead BSDF sample; nothing further can contribute
            {
                break;
            }

            scatterRayCone(payload.rayCone, surfMaterial, surfBsdfSample, wo_WS, surfNor_WS);

            setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, surfBsdfSample.wi_WS, true /*faceforwardNormal*/);

            bounceBsdfPdf = surfBsdfSample.pdf;
            bounceWasSpecular = surfBsdfSample.wasSpecular;
            bounceAcceptedBacksideLight = surfMaterial.acceptsBacksideLight();
        } // !isPassthrough

        ray.TMin = 0.f;
        ray.TMax = RAY_DEFAULT_TMAX;

        payload.flags &= PAYLOAD_FLAG_UNDERWATER; // reset all payload flags except PAYLOAD_FLAG_UNDERWATER
        payload.waterEntryT = RAY_DEFAULT_TMAX;
        payload.waterExitT = RAY_DEFAULT_TMAX;
#if SHARC_QUERY
        sharcCount(2);
        if (pathDepth == 0)
        {
            sharcCount(6); // BSDF rays actually launched from the primary surface
            if (primaryGlass)
                sharcCount(9);
        }
        ++tracedBounces;
#endif
        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const float hitDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            payload.rayCone.width = getRayConeWidthAtDistance(payload.rayCone, hitDistance);
            surfMaterial = getHitMaterial(payload, payload.rayCone.width);

        }

        const float3 segmentAbsorption = computeSegmentAbsorption(payload, ray.Origin, ray.Direction);

        // Bounces at pathDepth > 1 get only transmittance, no in-scattering.
        const uint numFogSteps = (pathDepth <= 1) ? max(renderParams.fogMarchSteps / 2, 1u) : 0u;
        const float fogTransmittance = applySegmentFog(payload, ray.Origin, ray.Direction, numFogSteps, pathColor);

        payload.pathWeight *= segmentAbsorption;

        if (isOrphanWaterBackfaceHit(payload))
        {
#if SHARC_UPDATE
            SharcUpdateMiss(makeSharcParameters(), sharcState, pathColor);
#endif
            break;
        }

        const bool didMiss = !bool(payload.flags & PAYLOAD_FLAG_DID_HIT);
        const float3 missDomeLightColor = didMiss ? getDomeLightColor(ray.Direction) : float3(0.f, 0.f, 0.f);

        if (pathDepth == 0)
        {
            // at this point, ptDiffuseAlbedo = first bounce path weight

            if (bounceWasSpecular)
            {
                if (!bool(renderParams.doPathSplitting))
                {
                    ptDiffuseAlbedo = 0.f;
                }
                else
                {
                    float3 secondHitDiffuseAlbedo = 0.f;
                    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
                    {
                        const PerTriangleData secondHitPerTriData =
                            perTriDatas[instanceDatas[payload.hitInfo.instanceId].perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
                        const TexSampleCtx secondHitTexCtx = makeTintedTexSampleCtx(
                            secondHitPerTriData, payload.rayCone.width, payload.hitInfo.hitPos_WS);
                        if (surfMaterial.hasDiffuse())
                        {
                            secondHitDiffuseAlbedo += getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, secondHitTexCtx).rgb;
                        }
                        if (surfMaterial.hasEmission())
                        {
                            secondHitDiffuseAlbedo +=
                                applyReinhard(getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, secondHitTexCtx));
                        }
                    }
                    const bool secondHitHasDiffuseAlbedo = any(secondHitDiffuseAlbedo > 0.f);

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

            pathColor += domeLightContrib;
#if SHARC_DECOMPOSE
            if (pathDepth == 0)
                breakdown.firstRayEmission += domeLightContrib;
            else
                breakdown.laterEmission += domeLightContrib;
#endif
#if SHARC_UPDATE
            SharcUpdateMiss(makeSharcParameters(), sharcState, pathColor);
#endif
            break;
        }
        else if (payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            break;
        }

        if (!SHARC_UPDATE && bool(renderParams.doPathSplitting) && pathDepth == 0 &&
            bounceWasSpecular) // TODO: support multiple specular bounces?
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
#if SHARC_UPDATE
        SharcUpdateMiss(makeSharcParameters(), sharcState, pathColor);
#endif
    }

    ptDiffuseAlbedo = saturate(ptDiffuseAlbedo + ptEmissiveAlbedo);
#if SHARC_QUERY
    if (sharcParams.debugMode == 1)
        pathColor = cacheHit ? float3(0, 1, 0) : float3(1, 0, 0);
    if (sharcParams.debugMode == 2)
        pathColor = lerp(float3(0, 1, 0), float3(1, 0, 0), saturate(tracedBounces / 8.f));
    if (sharcParams.debugMode > 0 && sharcParams.debugMode <= 4 && pathSplitIdx != 0)
        pathColor = 0;
#endif
}

[shader("raygeneration")]
void RayGeneration()
{
#if SHARC_UPDATE
    const uint2 tile = DispatchRaysIndex().xy;
    RandomNumberGenerator pixelRng = initRng(tile.x, tile.y, sharcParams.frameIndex, 17423);
    const uint2 pixelIdx =
        tile * sharcParams.downscale + uint2(pixelRng.nextUint(), pixelRng.nextUint()) % sharcParams.downscale;
    if (any(pixelIdx >= renderParams.renderSize))
        return;
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
    payload.rng = initRng(constantParams.rngSeed, 987654103, linearPixelIdx * (pathSplitIdx + 1), renderParams.frameNumber);
    payload.waterEntryT = RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone.angle = getRayConePixelAngle();
    payload.rayCone.width = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? payload.rayCone.angle * distance(cameraParams.pos_WS, payload.hitInfo.hitPos_WS)
        : 0.f;

    float3 pathColor = 0.f;
    float3 outPtDiffuseAlbedo = 0.f;
#if SHARC_QUERY
    if (sharcParams.debugMode == 4)
    {
        // Intentional primary-hit query for visualizing the cache itself. This does not
        // change the production query eligibility or add camera-direction data to it.
        if (pathSplitIdx == 0 && bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const Material material = getHitMaterial(payload, payload.rayCone.width);
            const PerTriangleData tri = perTriDatas[instanceDatas[payload.hitInfo.instanceId].perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
            const TexSampleCtx tex = makeTintedTexSampleCtx(tri, payload.rayCone.width, payload.hitInfo.hitPos_WS);
            SharcHitData hit = makeSharcHit(payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS,
                getMaterialBaseColor(material, payload.hitInfo.uv, tex).rgb);
            float3 cachedRadiance;
            if (SharcGetCachedRadiance(makeSharcParameters(), hit, cachedRadiance, false)) pathColor = cachedRadiance;
        }
    }
    else
#endif
    {
        PathRadianceBreakdown breakdown;
        pathTraceRay(payload, pixelIdx, pathSplitIdx, pathColor, outPtDiffuseAlbedo, breakdown);
#if SHARC_DECOMPOSE
        // Display only after tracing: all modes retain the same estimator, RNG draws,
        // cache queries and guide generation. The seven components sum to beauty.
        if (sharcParams.debugMode == 5) pathColor = breakdown.primaryNee;
        if (sharcParams.debugMode == 6) pathColor = breakdown.cached;
        if (sharcParams.debugMode == 7) pathColor = breakdown.firstRayEmission;
        if (sharcParams.debugMode == 8)
            pathColor = max(0.f, pathColor - breakdown.primaryNee - breakdown.cached - breakdown.firstRayEmission
                - breakdown.laterNee - breakdown.laterEmission - breakdown.visibleEmission);
        if (sharcParams.debugMode == 9) pathColor = breakdown.laterNee;
        if (sharcParams.debugMode == 10) pathColor = breakdown.laterEmission;
        if (sharcParams.debugMode == 11) pathColor = breakdown.visibleEmission;
#endif
    }
#if SHARC_QUERY
    if (sharcParams.debugMode == 3)
        pathColor = pathSplitIdx == 0 && bool(gbufferData.payloadFlags & PAYLOAD_FLAG_DID_HIT)
                        ? HashGridDebugColoredHash(gbufferData.hitInfo.hitPos_WS + float3(sharcParams.originDelta),
                                                   gbufferData.hitInfo.hitNor_WS,
                                                   makeSharcParameters().hashGridParameters)
                        : float3(0, 0, 0);
#endif

#if !SHARC_UPDATE
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
