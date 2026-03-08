/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "../rendering/common/common_structs.h"

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

    float3 pathColor;
    uint materialIdx;

    RandomNumberGenerator rng;
    float waterEntryT; // for REFRACTION_PASSTHROUGH rays: T where water was first entered (0 if starting underwater, RAY_DEFAULT_TMAX if not)
    float waterExitT;  // for REFRACTION_PASSTHROUGH rays: T where water was first exited (RAY_DEFAULT_TMAX if not yet exited)
    uint pad2;

    HitInfo hitInfo;
};
