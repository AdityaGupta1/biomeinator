// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_registers.h"

// Root constants shared by both sky LUT generation passes; the transmittance pass only uses
// lutUavIdx.
cbuffer SkyConstants : REGISTER_B(SKY, CONSTANTS)
{
    uint lutUavIdx;
    uint transmittanceLutSrvIdx;
    float animTime;
    float cameraY;
};

SamplerState lutSampler : REGISTER_S(SKY, LUT_SAMPLER);
