// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#include "materials/mipmap.hlsli"
#include "util/rng.hlsli"

#define PAYLOAD_FLAG_DID_HIT (1 << 0)
#define PAYLOAD_FLAG_BACKFACE_HIT (1 << 1)
#define PAYLOAD_FLAG_REFRACTION_PASSTHROUGH (1 << 2)
#define PAYLOAD_FLAG_UNDERWATER (1 << 3)
#define PAYLOAD_FLAG_IS_GBUFFER (1 << 4)

struct Payload
{
    uint flags;
    float3 pathWeight;

    uint materialIdx;
    RandomNumberGenerator rng;
    float waterEntryT; // for REFRACTION_PASSTHROUGH rays: T where water was first entered (0 if starting underwater, RAY_DEFAULT_TMAX if not)
    float waterExitT;  // for REFRACTION_PASSTHROUGH rays: T where water was first exited (RAY_DEFAULT_TMAX if not yet exited)

    RayCone rayCone;

    HitInfo hitInfo;
};
