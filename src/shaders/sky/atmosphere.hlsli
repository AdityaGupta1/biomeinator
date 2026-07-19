// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/math.hlsli"

// Earth atmosphere model from Hillaire, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique" (EGSR 2020), Table 1. All lengths are in meters (1 block = 1 meter) and
// all coefficients are per meter. Shared between the LUT generation passes and dome_light.hlsli
// so the parameterizations agree exactly.

static const float atmosphereGroundRadius = 6360.0e3f;
static const float atmosphereTopRadius = 6460.0e3f;

static const float3 rayleighScatteringCoeff = float3(5.802e-6f, 13.558e-6f, 33.1e-6f);
static const float rayleighScaleHeight = 8000.f;

static const float mieScatteringCoeff = 3.996e-6f;
// Table 1 prints "absorption 4.40" but that value is the extinction (Bruneton's scattering/0.9);
// Hillaire's reference implementation uses absorption ~= 0.444, giving a haze-like single-scatter
// albedo of 0.9 instead of soot-like 0.48.
static const float mieExtinctionCoeff = 4.44e-6f;
static const float mieScaleHeight = 1200.f;
static const float miePhaseG = 0.8f;

static const float3 ozoneAbsorptionCoeff = float3(0.650e-6f, 1.881e-6f, 0.085e-6f);
static const float ozoneCenterAltitude = 25000.f;
static const float ozoneHalfWidth = 15000.f;

static const float atmosphereGroundAlbedo = 0.3f;

static const float sunPeriodSeconds = 1200.f; // half above the horizon, half below
static const float sunTiltRadians = 23.5f * (M_PI / 180.f);
static const float sunPhaseOffsetRadians = M_PI / 4.f;

// The sun rides a great circle tilted towards +Z, rising at +X and setting at -X. Derived purely from
// animTime so that scrubbing time forwards or backwards lands on the same sky.
float3 computeSunDir_WS(const float animTime)
{
    const float angle = animTime * (M_TWO_PI / sunPeriodSeconds) + sunPhaseOffsetRadians;
    float sinAngle, cosAngle;
    sincos(angle, sinAngle, cosAngle);
    return float3(cosAngle, sinAngle * cos(sunTiltRadians), sinAngle * sin(sunTiltRadians));
}

// The camera is kept at least 1m above the ground sphere, where the horizon parameterization
// degenerates.
float atmosphereRadiusForCameraY(const float cameraY)
{
    return atmosphereGroundRadius + max(cameraY, 1.f);
}

struct MediumSample
{
    float3 rayleighScattering;
    float mieScattering;
    float3 extinction;
};

MediumSample sampleMedium(const float altitude)
{
    const float rayleighDensity = exp(-altitude / rayleighScaleHeight);
    const float mieDensity = exp(-altitude / mieScaleHeight);
    const float ozoneDensity = max(0.f, 1.f - abs(altitude - ozoneCenterAltitude) / ozoneHalfWidth);

    MediumSample result;
    result.rayleighScattering = rayleighScatteringCoeff * rayleighDensity;
    result.mieScattering = mieScatteringCoeff * mieDensity;
    result.extinction = result.rayleighScattering
        + (mieExtinctionCoeff * mieDensity)
        + (ozoneAbsorptionCoeff * ozoneDensity);
    return result;
}

float rayleighPhase(const float cosTheta)
{
    return (3.f / (16.f * M_PI)) * (1.f + cosTheta * cosTheta);
}

// Cornette-Shanks
float miePhase(const float cosTheta)
{
    const float g = miePhaseG;
    const float g2 = g * g;
    const float numerator = (1.f - g2) * (1.f + cosTheta * cosTheta);
    const float denominator = (2.f + g2) * pow(1.f + g2 - 2.f * g * cosTheta, 1.5f);
    return (3.f / (8.f * M_PI)) * numerator / denominator;
}

// Nearest intersection t >= 0 of the ray with a sphere of the given radius centered at the
// origin, or -1 if there is no such intersection.
float raySphereIntersectNearest(const float3 rayOrigin, const float3 rayDir, const float radius)
{
    const float b = dot(rayOrigin, rayDir);
    const float c = dot(rayOrigin, rayOrigin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.f)
    {
        return -1.f;
    }
    const float sqrtDiscriminant = sqrt(discriminant);
    const float t0 = -b - sqrtDiscriminant;
    if (t0 >= 0.f)
    {
        return t0;
    }
    const float t1 = -b + sqrtDiscriminant;
    return t1 >= 0.f ? t1 : -1.f;
}

// =============================================
// Transmittance LUT parameterization
// =============================================

// Bruneton-Neyret parameterization (BN08 §4): non-linear in the view zenith angle so texels
// concentrate near the horizon. r is the distance from the planet center and mu the cosine of
// the view zenith angle. Only valid for rays that don't hit the ground sphere.

float2 transmittanceLutRMuToUv(const float r, const float mu)
{
    const float H = sqrt(atmosphereTopRadius * atmosphereTopRadius - atmosphereGroundRadius * atmosphereGroundRadius);
    const float rho = sqrt(max(0.f, r * r - atmosphereGroundRadius * atmosphereGroundRadius));

    const float discriminant = r * r * (mu * mu - 1.f) + atmosphereTopRadius * atmosphereTopRadius;
    const float distToTop = max(0.f, -r * mu + sqrt(max(0.f, discriminant)));

    const float distMin = atmosphereTopRadius - r;
    const float distMax = rho + H;
    return float2((distToTop - distMin) / (distMax - distMin), rho / H);
}

void transmittanceLutUvToRMu(const float2 uv, out float r, out float mu)
{
    const float H = sqrt(atmosphereTopRadius * atmosphereTopRadius - atmosphereGroundRadius * atmosphereGroundRadius);
    const float rho = H * uv.y;
    r = sqrt(rho * rho + atmosphereGroundRadius * atmosphereGroundRadius);

    const float distMin = atmosphereTopRadius - r;
    const float distMax = rho + H;
    const float distToTop = distMin + uv.x * (distMax - distMin);
    mu = distToTop == 0.f ? 1.f : (H * H - rho * rho - distToTop * distToTop) / (2.f * r * distToTop);
    mu = clamp(mu, -1.f, 1.f);
}

// Per-wavelength transmittance from a point at radius r to the atmosphere top along a ray with
// zenith cosine mu.
float3 sampleTransmittanceLut(Texture2D<float4> transmittanceLut,
                              SamplerState lutSampler,
                              const float r,
                              const float mu)
{
    return transmittanceLut.SampleLevel(lutSampler, transmittanceLutRMuToUv(r, mu), 0).rgb;
}

// =============================================
// Multi-scattering LUT parameterization
// =============================================

// Paper §5.5.2: (sun zenith cosine, altitude). Ψms is isotropic and the medium only varies with
// altitude, so it's valid for any view point and light direction.

float2 multiScatteringLutRMuSunToUv(const float r, const float muSun)
{
    const float u = 0.5f + 0.5f * muSun;
    const float v = saturate((r - atmosphereGroundRadius) / (atmosphereTopRadius - atmosphereGroundRadius));
    return float2(u, v);
}

// Ψms (unit sr^-1): multiplied by a directional light's illuminance and the local scattering
// coefficient to get the multiple scattering contribution (Eq. 10-11).
float3 sampleMultiScatteringLut(Texture2D<float4> multiScatteringLut,
                                SamplerState lutSampler,
                                const float r,
                                const float muSun)
{
    return multiScatteringLut.SampleLevel(lutSampler, multiScatteringLutRMuSunToUv(r, muSun), 0).rgb;
}

// =============================================
// Sky-view LUT parameterization
// =============================================

// Lat/long map around the camera. Longitude (u) is the view azimuth relative to the sun azimuth
// so the sun-relative sky is stable as the sun moves, with the sun at u = 0.5. Latitude uses the
// paper's quadratic mapping (§5.3) to concentrate texels at the horizon (v = 0.5).

float2 skyViewDirToUv(const float3 dir_WS, const float3 sunDir_WS)
{
    const float deltaAzimuth = atan2(dir_WS.z, dir_WS.x) - atan2(sunDir_WS.z, sunDir_WS.x);
    float u = deltaAzimuth * (1.f / M_TWO_PI) + 0.5f;
    u = frac(u);

    const float latitude = asin(clamp(dir_WS.y, -1.f, 1.f));
    const float v = 0.5f + 0.5f * sign(latitude) * sqrt(abs(latitude) / (M_PI / 2.f));

    return float2(u, v);
}

float3 skyViewUvToDir(const float2 uv, const float3 sunDir_WS)
{
    const float azimuth = (uv.x - 0.5f) * M_TWO_PI + atan2(sunDir_WS.z, sunDir_WS.x);

    const float latitudeParam = 2.f * uv.y - 1.f;
    const float latitude = sign(latitudeParam) * latitudeParam * latitudeParam * (M_PI / 2.f);

    float sinLatitude, cosLatitude;
    sincos(latitude, sinLatitude, cosLatitude);
    float sinAzimuth, cosAzimuth;
    sincos(azimuth, sinAzimuth, cosAzimuth);
    return float3(cosLatitude * cosAzimuth, sinLatitude, cosLatitude * sinAzimuth);
}
