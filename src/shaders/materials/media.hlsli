// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

static const float3 waterSigmaA = float3(0.35f, 0.06f, 0.02f) * 0.4f;

// Indexed by MEDIUM_*. A medium boundary face refracts by the ratio of the IORs on its two sides, so
// the same glass refracts less in water than in air.
static const float mediumIors[MEDIUM_COUNT] = {
    1.f,
    1.33f,
    1.31f,
    1.55f, // quartz-ish
};

// Absorption along path segments inside each medium. Ice absorbs about as much as water does.
static const float3 mediumSigmaAs[MEDIUM_COUNT] = {
    float3(0.f, 0.f, 0.f),
    waterSigmaA,
    waterSigmaA,
    float3(0.f, 0.f, 0.f),
};
