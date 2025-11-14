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

#include "nvapi_includes.hlsli"

#include "global_params.hlsli"
#include "light_sampling.hlsli"
#include "materials.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"
#include "restir.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbuffer : REGISTER_T(PT_REGISTER_GBUFFER, PT_REGISTER_SPACE);
StructuredBuffer<RisSample> risSamplesIn : REGISTER_T(PT_REGISTER_RIS_SAMPLES_IN, PT_REGISTER_SPACE);
RWStructuredBuffer<float4> pathTracingRawBuffer : REGISTER_U(PT_REGISTER_PATH_TRACING_RAW_BUFFER, PT_REGISTER_SPACE);

float balanceHeuristic(const float pdfA, const float pdfB)
{
    return pdfA / (pdfA + pdfB);
}

void pathTraceRay(inout Payload payload)
{
    const uint pathSplitIdx = getPathSplitIdx();
    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(payload.pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialIdx == MATERIAL_IDX_INVALID)
    {
        return;
    }

    bool previousWasSpecular = false;
    bool hasEncounteredNonDeltaSurface = false;

    for (uint pathDepth = 0; pathDepth < renderParams.maxPathDepth; ++pathDepth)
    {
        Material surfMaterial = materials[payload.materialIdx];

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and multiply by Fresnel reflectance
        // In RIS mode, only include emission if this is the first bounce (pathDepth == 0) or the previous event was a delta event (specular)
        if ((samplingMode != SamplingMode::RIS || pathDepth == 0 || previousWasSpecular) && (pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
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

        const bool isNonDeltaSurface = !surfMaterial.isDelta();

        if (samplingMode == SamplingMode::MIS || samplingMode == SamplingMode::RIS)
        {
            if (isNonDeltaSurface)
            {
                DirectLightingSample lightSample;
                if (samplingMode == SamplingMode::RIS)
                {
                    const bool isFirstNonDeltaSurface = !hasEncounteredNonDeltaSurface;

                    // it might be kind of sus that this is under the conditional isNonDeltaSurface, but it seems to work well for now
                    NvReorderThread(isFirstNonDeltaSurface ? 0 : 1, 1);

                    RisSample risSample = generateDirectLightingRisSample(surfPos_WS, surfNor_WS, surfMaterial, payload.hitInfo.uv, wo_WS, isFirstNonDeltaSurface, payload.rng);
                    lightSample = sampleDirectLightingRis(risSample, surfPos_WS, surfNor_WS);
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

                    float3 contribution = payload.pathWeight * bsdfVal * absCosTheta(lightSample.wi_WS, surfNor_WS) * lightSample.Le;

                    if (samplingMode == SamplingMode::RIS)
                    {
                        contribution *= lightSample.pdfOrW_Y;
                    }
                    else
                    {
                        const float lightSampleBsdfPdf = bsdfPdf(surfMaterial, wo_WS, lightSample.wi_WS, surfNor_WS);
                        contribution /= (lightSample.pdfOrW_Y + lightSampleBsdfPdf); // balance heuristic (light pdf cancels out)
                    }

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

        ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
        ray.Direction = surfBsdfSample.wi_WS;
        ray.TMin = 0.f;
        ray.TMax = 10000.f;

        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_PATH_FINISHED) || payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            return;
        }

        if (pathDepth == 0 && surfBsdfSample.wasSpecular)
        {
            RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
            specularHitDistanceTarget[payload.pixelIdx] = distance(surfPos_WS, payload.hitInfo.hitPos_WS);

            const float3 secondBounceNor_WS = faceforward(payload.hitInfo.hitNor_WS, -ray.Direction);
            RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
            normalsAndRoughnessTarget[payload.pixelIdx].xyz = secondBounceNor_WS;
        }

        if (samplingMode == SamplingMode::MIS)
        {
            const Material hitMaterial = materials[payload.materialIdx];
            if (hitMaterial.hasEmission() && !surfBsdfSample.wasSpecular)
            {
                const float bsdfSampleLightPdf = lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);
                payload.pathWeight *= (surfBsdfSample.pdf / (surfBsdfSample.pdf + bsdfSampleLightPdf)); // balance heuristic
            }
            // if BSDF sampling didn't hit a light, lightPdf = 0 (I think) so misWeight = 1
        }

        previousWasSpecular = surfBsdfSample.wasSpecular;
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

    Payload payload = gbufferPayload;
    payload.rng = initRandomSampler3(uint3(constantParams.rngSeed + pathSplitIdx, linearPixelIdx, renderParams.frameNumber));

    pathTraceRay(payload);

    const float3 colorPreTonemap = payload.pathColor;
    const uint writePixelIdx = linearPixelIdx * (renderParams.enablePathSplitting ? 2 : 1) + pathSplitIdx;
    if ((AntialiasingMode)renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE && renderParams.accumulatedFrameNumber > 0)
    {
        pathTracingRawBuffer[writePixelIdx].xyz += colorPreTonemap;
    }
    else
    {
        pathTracingRawBuffer[writePixelIdx].xyz = colorPreTonemap;
    }
}
