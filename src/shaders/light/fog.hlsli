// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// NOTE: needs the sun constants, so this file must be included after dome_light.hlsli.

#include "../rendering/common/common_settings.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "light/dome_light.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"

static const float fogSeaLevelY = float(SEA_LEVEL);
static const float fogUndergroundRampBlocks = 24.f;

// Fog density profile in true world-space Y: zero below (seaLevel - fogUndergroundRampBlocks), linear ramp up to
// seaLevel, exponential falloff above. Linear (not smoothstep) in the ramp so the optical-depth integral stays
// closed-form; the ramp is mostly underground anyway.
float getFogDensity(const float y)
{
    const float rampBottomY = fogSeaLevelY - fogUndergroundRampBlocks;
    if (y <= rampBottomY)
    {
        return 0.f;
    }
    if (y <= fogSeaLevelY)
    {
        return renderParams.fogSigmaS * (y - rampBottomY) / fogUndergroundRampBlocks;
    }
    return renderParams.fogSigmaS * exp(-(y - fogSeaLevelY) / renderParams.fogScaleHeight);
}

// Closed-form optical depth of a segment through the fog profile, split at the two zone
// boundary heights. origin_WS is shader world space; true world Y adds globalInstanceOffset.
float computeFogOpticalDepth(const float3 origin_WS, const float3 dir, const float dist)
{
    const float sigmaS = renderParams.fogSigmaS;
    const float scaleHeight = renderParams.fogScaleHeight;
    const float rampBottomY = fogSeaLevelY - fogUndergroundRampBlocks;

    const float y0 = origin_WS.y + float(cameraParams.globalInstanceOffset.y);
    const float dy = dir.y;

    if (abs(dy) < 1e-4f) // near-horizontal: constant-height limit
    {
        return getFogDensity(y0) * dist;
    }

    const float invDy = rcp(dy);
    const float tAtRampBottom = (rampBottomY - y0) * invDy;
    const float tAtSeaLevel = (fogSeaLevelY - y0) * invDy;

    float opticalDepth = 0.f;

    // ramp zone: density is linear in Y, so the integral is average density times sub-length
    const float rampT0 = clamp(min(tAtRampBottom, tAtSeaLevel), 0.f, dist);
    const float rampT1 = clamp(max(tAtRampBottom, tAtSeaLevel), 0.f, dist);
    if (rampT1 > rampT0)
    {
        const float yMid = y0 + ((rampT0 + rampT1) * 0.5f) * dy;
        const float densityYMid = sigmaS * ((yMid - rampBottomY) / fogUndergroundRampBlocks);
        opticalDepth += densityYMid * (rampT1 - rampT0);
    }

    // exponential zone
    const float expT0 = (dy > 0.f) ? clamp(tAtSeaLevel, 0.f, dist) : 0.f;
    const float expT1 = (dy > 0.f) ? dist : clamp(tAtSeaLevel, 0.f, dist);
    if (expT1 > expT0)
    {
        const float yA = y0 + dy * expT0;
        const float yB = y0 + dy * expT1;
        const float densityYA = sigmaS * exp(-(yA - fogSeaLevelY) / scaleHeight);
        const float densityYB = sigmaS * exp(-(yB - fogSeaLevelY) / scaleHeight);
        opticalDepth += (densityYA - densityYB) * scaleHeight * invDy;
    }

    return opticalDepth;
}

float computeFogTransmittance(const float3 origin_WS, const float3 dir, const float dist)
{
    return exp(-computeFogOpticalDepth(origin_WS, dir, dist));
}

float henyeyGreensteinPhase(const float cosAngle, const float g)
{
    const float g2 = g * g;
    const float denom = 1.f + g2 - 2.f * g * cosAngle;
    return (1.f - g2) / (4.f * M_PI * denom * sqrt(denom));
}

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
        if (material.hasGlossyTransmission())
        {
            continue; // transmissive surfaces (e.g. water) let sunlight through
        }

        if (!material.hasDiffuse() || material.baseColorTextureId == TEXTURE_ID_INVALID)
        {
            query.CommitNonOpaqueTriangleHit();
            continue;
        }

        // Mip 0 and a fixed threshold: occlusion is a boolean, so no ray cone or stochastic
        // alpha handling needed here.
        const PerTriangleData perTriData =
            perTriDatas[instanceData.perTriDatasBufferOffset + query.CandidatePrimitiveIndex()];
        const float4 baseColor = getMaterialBaseColorAtHit(material,
                                                           instanceData,
                                                           perTriData,
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

    // Use finite-disk visibility at each scattering point, not a binary test at
    // the camera (which made the entire fog volume switch off at sunset).
    {
        const float phase = henyeyGreensteinPhase(dot(dir, sunDir_WS), renderParams.fogG);

        const float stepLength = dist / numSteps;
        float3 sunScatter = 0.f;
        const float3 sunRight = normalize(cross(sunDir_WS, abs(sunDir_WS.y) < 0.99f ? float3(0.f,1.f,0.f) : float3(1.f,0.f,0.f)));
        const float3 sunUp = cross(sunRight, sunDir_WS);
        for (uint stepIdx = 0; stepIdx < numSteps; ++stepIdx)
        {
            // Fixed midpoint planes alias spatial cloud shadows into parallel
            // bands, particularly where the ray's exit switches box faces.
            // Stratify along each ray so reconstruction integrates the shadow
            // field instead of preserving eight copies of its silhouette.
            const float t = (stepIdx + rng.nextFloat()) * stepLength;
            const float3 stepPos_WS = origin_WS + dir * t;
            const float density = getFogDensity(stepPos_WS.y + globalOffsetY);
            if (density <= 0.f)
            {
                continue;
            }

            const float3 sunEnergy = getVolumeSunEnergy(sunDir_WS, stepPos_WS.y + globalOffsetY);
            if (!any(sunEnergy > 0.f))
            {
                continue;
            }

            const float viewTransmittance = computeFogTransmittance(origin_WS, dir, t);
            float visibility = 0.f;
            [unroll] for (uint diskIdx = 0; diskIdx < 4; ++diskIdx)
            {
                const float2 offset = float2((diskIdx & 1u) ? 0.5f : -0.5f, (diskIdx & 2u) ? 0.5f : -0.5f);
                const float3 lightDir = normalize(sunDir_WS + acos(sunCosTheta) * (sunRight * offset.x + sunUp * offset.y));
                if (!isRayOccluded(stepPos_WS, lightDir))
                {
                    const float sunVolumeDist = getDistanceToVoxelBounds(stepPos_WS, lightDir);
                    visibility += 0.25f * computeFogTransmittance(stepPos_WS, lightDir, sunVolumeDist);
                }
            }
            sunScatter += viewTransmittance * density * visibility * cloudSunTransmittance(stepPos_WS, sunDir_WS) * stepLength * sunEnergy;
        }

        // Solar energy already includes disk visibility and atmospheric
        // transmittance at each scattering point, including sunset reddening.
        inScatter = sunScatter * phase;
    }

    // NOTE: no visibility check, so this also brightens enclosed spaces (cave interiors)
    // with sky-colored haze; fogAmbientStrength is the artistic control for how much.
    inScatter += renderParams.fogAmbientStrength * (1.f - segmentTransmittance) * getSkyColor(float3(0.f, 1.f, 0.f)) * lerp(0.65f, 1.f, cloudSunTransmittance(origin_WS, sunDir_WS));

    return inScatter;
}
