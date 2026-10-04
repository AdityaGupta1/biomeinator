// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#define MEDIUM_IOR_ENTRY(name, ior, sigmaA) ior,
#define MEDIUM_SIGMA_A_ENTRY(name, ior, sigmaA) sigmaA,
static const float mediumIors[(uint)Medium::COUNT] = { MEDIA_TABLE(MEDIUM_IOR_ENTRY) };
static const float3 mediumSigmaAs[(uint)Medium::COUNT] = { MEDIA_TABLE(MEDIUM_SIGMA_A_ENTRY) };
#undef MEDIUM_IOR_ENTRY
#undef MEDIUM_SIGMA_A_ENTRY

// A medium boundary face refracts by the ratio of the IORs on its two sides, so the same glass refracts
// less in water than in air
float getMediumIor(const Medium medium)
{
    return mediumIors[(uint)medium];
}

// Absorption along path segments inside the medium
float3 getMediumSigmaA(const Medium medium)
{
    return mediumSigmaAs[(uint)medium];
}
