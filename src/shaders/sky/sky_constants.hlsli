// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_cloud_settings.h"

// Root constants shared by both sky LUT generation passes; the transmittance pass only uses
// lutUavIdx.
cbuffer SkyConstants : REGISTER_B(SKY, CONSTANTS)
{
    uint lutUavIdx;
    uint transmittanceLutSrvIdx;
    uint multiScatteringLutSrvIdx;
    float animTime;
    float cameraY;
    uint cloudNoiseIdx;
    uint cloudShapeIdx;
    float cloudCoverage;
    float cloudDensity;
    uint sunSampleFrame;
    float2 cloudPadding;
    CloudSettings cloud;
};

SamplerState lutSampler : REGISTER_S(SKY, LUT_SAMPLER);
SamplerState cloudNoiseSampler : REGISTER_S(SKY, NOISE_SAMPLER);
