// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once
#include "common/global_params.hlsli"
#include "sky/atmosphere.hlsli"
#include "sky/sun_sampling.hlsli"

// Calibrated against the previous hand-tuned sun (radiance 16000 over the oversized disk's solid
// angle, ~10 lux) so overall exposure and tonemapping don't shift drastically.
static const float3 sunIlluminance = float3(10.f, 10.f, 10.f);

// Constant floor on every sky lookup, sized so nights aren't pitch black until the moon exists;
// it tints the daytime sky slightly too. Added at the lookup rather than baked into the sky-view
// LUT so it's trivial to delete when the moon lands.
static const float3 ambientSkyLight = float3(0.015f, 0.0225f, 0.0375f);

SamplerState skyLutSampler : REGISTER_S(RT, LUT_SAMPLER);
SamplerState skyViewSampler : REGISTER_S(RT, SKY_VIEW_SAMPLER);

float3 getSunDir_WS()
{
    return computeSunDir_WS(renderParams.animTime);
}

bool isInSun(float3 wi_WS)
{
    return dot(wi_WS, getSunDir_WS()) >= sunCosTheta;
}

float getCameraAtmosphereRadius()
{
    return atmosphereRadiusForCameraY(cameraParams.pos_WS.y + cameraParams.globalInstanceOffset.y);
}

float3 getSkyColor(float3 wi_WS)
{
    Texture2D<float4> skyViewLut = ResourceDescriptorHeap[heapIndices.srv.skyViewLutIdx];
    const float2 uv = skyViewDirToUv(wi_WS, getSunDir_WS());
    // Shared by visible sky, indirect surface lighting, and volume ambient.
    // Solar-disk radiance and direct solar energy use separate functions.
    return renderParams.skyStrength * (skyViewLut.SampleLevel(skyViewSampler, uv, 0).rgb * sunIlluminance + ambientSkyLight);
}

// True if the ray from the camera towards wi_WS is occluded by the virtual planet. The
// transmittance parameterization only covers rays that don't hit the ground sphere, and isInSun
// alone would show the disk through the horizon at night.
bool isSunOccluded(float3 wi_WS)
{
    const float r = getCameraAtmosphereRadius();
    return raySphereIntersectNearest(float3(0.f, r, 0.f), wi_WS, atmosphereGroundRadius) >= 0.f;
}

float3 getSunColor(float3 wi_WS)
{
    Texture2D<float4> transmittanceLut = ResourceDescriptorHeap[heapIndices.srv.transmittanceLutIdx];
    const float3 transmittance = sampleTransmittanceLut(transmittanceLut, skyLutSampler, getCameraAtmosphereRadius(), wi_WS.y);
    return sunIlluminance * transmittance / sunSolidAngle;
}

// Energy / sampling PDF for ONE uniformly sampled solar direction. Planet
// visibility is tested at the scattering point, not at the camera. Do not also
// multiply by the visible disk fraction: random disk samples integrate that.
float3 getVolumeSunEnergy(float3 sunDir, float worldY)
{
    const float r = atmosphereRadiusForCameraY(worldY);
    if (raySphereIntersectNearest(float3(0.f, r, 0.f), sunDir, atmosphereGroundRadius) >= 0.f)
        return 0.f;
    Texture2D<float4> lut = ResourceDescriptorHeap[heapIndices.srv.transmittanceLutIdx];
    return sunIlluminance * sampleTransmittanceLut(lut, skyLutSampler, r, sunDir.y);
}
