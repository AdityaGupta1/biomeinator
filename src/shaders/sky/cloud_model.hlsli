// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#ifndef CLOUD_CONFIG
#define CLOUD_CONFIG renderParams.cloud
#endif
#ifndef CLOUD_TIME
#define CLOUD_TIME renderParams.animTime
#endif
#define cloudBase (CLOUD_CONFIG.baseHeight)
#define cloudTop (CLOUD_CONFIG.baseHeight + CLOUD_CONFIG.thickness)
#define cloudPeriod (CLOUD_CONFIG.period)
#define cloudMaxDistance (CLOUD_CONFIG.maxDistance)
#define cloudWind float2(CLOUD_CONFIG.windX, CLOUD_CONFIG.windZ)

// An orthonormal horizontal basis keeps the tile grid off the world's cardinal
// axes. Density, erosion and cached light must all use this same basis.
float3 cloudToField(float3 p)
{
    return float3(0.8f*p.x + 0.6f*p.z, p.y, -0.6f*p.x + 0.8f*p.z);
}

float3 cloudFromField(float3 p)
{
    return float3(0.8f*p.x - 0.6f*p.z, p.y, 0.6f*p.x + 0.8f*p.z);
}

float3 cloudFieldUv(float3 p)
{
    p = cloudToField(p);
    return float3(p.x / cloudPeriod, (p.y - cloudBase) / (cloudTop - cloudBase), p.z / cloudPeriod);
}

#include "sky/cloud_reference_noise.hlsli"

// Blender uses Z-up: Mapping(Position)*0.07. One full layer is 10 Blender
// units; the horizontal cache spans 16 mapped units, centered on the origin.
float3 cloudReferencePosition(float3 p)
{
    p = cloudToField(p);
    return float3(p.x/cloudPeriod*16.f,p.z/cloudPeriod*16.f,
                  (p.y-cloudBase)/CLOUD_CONFIG.thickness*0.7f);
}

float cloudBodyMargin(float3 p, Texture3D<float> shape, SamplerState samplerLinear, float coverage)
{
    float3 uv = cloudFieldUv(p);
    if (uv.y < 0.f || uv.y > 1.f)
        return 0.f;
    const float distance = shape.SampleLevel(samplerLinear,float3(uv.x+0.5f,0.5f,uv.z+0.5f),0);
    // Map Range [0,10], then the EASE ramp [0,0.17727293], times 6.1.
    const float height = cloudReferenceHeight(uv.y*0.7f,CLOUD_CONFIG.heightRampEnd,CLOUD_CONFIG.heightGain);
    // Coverage 0.5 is exactly the supplied graph. The margin is kept unclamped
    // above one so the final ramp is evaluated AFTER adding the fine noise.
    return max(0.f,CLOUD_CONFIG.densityRampEnd+1.f/256.f-distance-height+(coverage-0.5f)*0.4f);
}

float cloudDetailDensity(float margin, float3 p)
{
    const float fine = cloudReferenceFine(cloudReferencePosition(p),CLOUD_CONFIG.fineScale,CLOUD_CONFIG.fineDetail,CLOUD_CONFIG.fineRoughness,CLOUD_CONFIG.fineTime+CLOUD_TIME*CLOUD_CONFIG.fineSpeed);
    return cloudReferenceRamp(CLOUD_CONFIG.densityRampEnd+1.f/256.f-margin+CLOUD_CONFIG.fineStrength*fine,CLOUD_CONFIG.densityRampStart,CLOUD_CONFIG.densityRampEnd);
}
