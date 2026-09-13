// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#ifndef CLOUD_USE_NOISE_CACHE
#define CLOUD_USE_NOISE_CACHE 1
#endif
#if CLOUD_USE_NOISE_CACHE
#include "sky/cloud_noise_cache.hlsli"
#endif
#include "sky/cloud_reference_noise.hlsli"

float3 cloudPosition(float3 origin_WS)
{
    float3 p = origin_WS + float3(cameraParams.globalInstanceOffset);
    p.xz -= float2(renderParams.cloud.windX, renderParams.cloud.windZ) * renderParams.animTime;
    return p;
}

float cloudDensity(float3 p, bool fineDetail)
{
    const CloudSettings c = renderParams.cloud;
    const float height = (p.y - c.baseHeight) / c.thickness;
    if (height < 0.f || height > 1.f)
        return 0.f;

    // Normalize the reference proportions; world dimensions are independent settings.
    const float3 q = float3(p.xz * (16.f / c.period), height * 0.7f);
    const float bottom = c.bottomGain * saturate(1.f - height / c.bottomWidth);
    const float top = cloudReferenceHeight(q.z, c.heightRampEnd, c.heightGain);
    const float threshold = (c.coverage - 0.5f) * 0.4f;
    // Rotating the 3D gradients preserves their length: the normalized fBM's
    // signed value is bounded by sqrt(6) * 0.982, including during evolution.
    const float fineBound = fineDetail ? 2.5f * c.fineStrength : 0.f;
    // Smooth F1 cannot fall below minus its blend width, even for coincident sites.
    if (top + bottom - threshold - fineBound - 0.5f * c.voronoiSmoothness >= c.densityRampEnd + 1.f / 256.f)
        return 0.f;
    const float2 warp = cloudReferenceWarp(q.xy, c.warpScale, c.warpDetail, c.warpRoughness,
        c.warpTime + renderParams.animTime * c.warpSpeed);
    float field = cloudSmoothF1(q.xy + warp * c.warpStrength, c.voronoiSmoothness,
        c.voronoiRandomness, c.voronoiTime + renderParams.animTime * c.voronoiSpeed)
        + top + bottom - threshold;
    // Centered detail can add density beyond the base field.
    if (field - fineBound >= c.densityRampEnd + 1.f / 256.f)
        return 0.f;
    if (fineDetail && c.fineStrength > 0.f)
    {
        const float noise = cloudReferenceFine(q, c.fineScale, c.fineDetail, c.fineRoughness,
            c.fineTime + renderParams.animTime * c.fineSpeed);
        field += c.fineStrength * (2.f * noise - 1.f);
    }
    return c.density * cloudReferenceRamp(field, c.densityRampStart, c.densityRampEnd);
}
