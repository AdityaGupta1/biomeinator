// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_settings.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "light/dome_light.hlsli"
#include "light/fog_density.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"
#include "util/sampling.hlsli"

// Inline ray query instead of TraceRay: no payload or shader-table indirection on the fog
// march's hot loop, and alpha-cutout foliage can be tested per candidate so leaves don't
// occlude as solid quads.
bool isRayOccluded(const float3 pos_WS, const float3 dir)
{
    RayDesc ray;
    ray.Origin = pos_WS;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = RAY_DEFAULT_TMAX;

    // The OMM opt-in is required because traversal over OMM-linked terrain is otherwise
    // undefined; with OMMs linked, terrain cutout resolves in hardware and never surfaces as
    // a candidate below (the alpha test there remains for the non-OMM fallback and glTF mode)
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
             RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> query;
    query.TraceRayInline(raytracingAcs, RAY_FLAG_NONE, 0xFF, ray);

    // SKIP_PROCEDURAL_PRIMITIVES means every candidate is a non-opaque triangle. Committing
    // one ends traversal via ACCEPT_FIRST_HIT_AND_END_SEARCH.
    while (query.Proceed())
    {
        const InstanceData instanceData = instanceDatas[query.CandidateInstanceID()];
        if (instanceData.materialIdx == MATERIAL_IDX_INVALID)
        {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }

        const Material material = materials[instanceData.materialIdx];
        // Only the scalar roughness factor is considered: resolving a roughness map's specular
        // texels would cost a texture sample per candidate, which is far too expensive here.
        if (material.isDeltaTransmission())
        {
            continue; // perfectly specular transmitters (e.g. water) let sunlight through
        }

        if (!material.hasDiffuse() || material.baseColorTextureId == TEXTURE_ID_INVALID)
        {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }

        // Mip 0 and a fixed threshold: occlusion is a boolean, so no ray cone or stochastic
        // alpha handling needed here.
        const PerFaceData perFaceData =
            loadPerFaceData(instanceData, query.CandidatePrimitiveIndex());
        const float4 baseColor = getMaterialBaseColorAtHit(material,
                                                           instanceData,
                                                           perFaceData,
                                                           query.CandidatePrimitiveIndex(),
                                                           query.CandidateTriangleBarycentrics(),
                                                           0.f);
        if (baseColor.a >= 0.5f)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    return query.CommittedStatus() != COMMITTED_NOTHING;
}

// Marches the fog along a segment, accumulating single-scattered sunlight plus a cheap
// analytic sky ambient term (aerial perspective). Returns radiance to be multiplied by the
// path weight at the segment start and outputs the segment's fog transmittance, which the
// caller applies to pathWeight separately. numSteps == 0 skips the march and the ambient
// term, returning zero radiance (but still a valid transmittance).
float3 computeFogInScatter(const float3 origin_WS,
                           const float3 dir,
                           const float dist,
                           const uint numSteps,
                           inout RandomNumberGenerator rng,
                           out float segmentTransmittance)
{
    segmentTransmittance = computeFogTransmittance(origin_WS, dir, dist);
    if (numSteps == 0)
    {
        return float3(0.f, 0.f, 0.f);
    }

    const float globalOffsetY = float(cameraParams.globalInstanceOffset.y);
    const float3 sunDir_WS = getSunDir_WS();

    float3 inScatter = float3(0.f, 0.f, 0.f);

    // Below the horizon the sun contributes nothing, so skip the march entirely at night.
    const float3 sunLight = getAttenuatedSunIlluminance(sunDir_WS, origin_WS.y + globalOffsetY);
    if (any(sunLight > 0.f))
    {
        const float phase = henyeyGreensteinPhase(dot(dir, sunDir_WS), renderParams.fogG);

        const float stepLength = dist / numSteps;
        float sunScatter = 0.f;
        for (uint stepIdx = 0; stepIdx < numSteps; ++stepIdx)
        {
            const float t = (stepIdx + rng.nextFloat()) * stepLength;
            const float3 stepPos_WS = origin_WS + dir * t;
            const float density = getFogDensity(stepPos_WS.y + globalOffsetY);
            if (density <= 0.f)
            {
                continue;
            }

            // The sun is a disk, not a point, so shadowing is tested against a fresh direction
            // within its cap each step. The weighting below is already the uniform cap estimator
            // (Le / pdf, with pdf = 1 / sunSolidAngle), so no extra sample weight is needed.
            const float3 sunSampleDir_WS = sampleSunDirection(sunDir_WS, rng);
            if (isRayOccluded(stepPos_WS, sunSampleDir_WS))
            {
                continue;
            }

            const float viewTransmittance = computeFogTransmittance(origin_WS, dir, t)
                * cloudTransmittance(origin_WS, dir, t);
            const float sunVolumeDist = getDistanceToVoxelBounds(stepPos_WS, sunSampleDir_WS);
            const float sunTransmittance = computeFogTransmittance(stepPos_WS, sunSampleDir_WS, sunVolumeDist)
                * cloudTransmittance(stepPos_WS, sunSampleDir_WS, cloudUnboundedDistance);
            sunScatter += viewTransmittance * density * sunTransmittance * stepLength;
        }

        // Atmospheric transmittance is folded into sunLight, reddening the shafts at sunset.
        inScatter = sunScatter * phase * sunLight;
    }

    // NOTE: no visibility check, so this also brightens enclosed spaces (cave interiors)
    // with sky-colored haze; fogAmbientStrength is the artistic control for how much.
    inScatter += renderParams.fogAmbientStrength * (1.f - segmentTransmittance) * getSkyColor(float3(0.f, 1.f, 0.f));

    return inScatter;
}
