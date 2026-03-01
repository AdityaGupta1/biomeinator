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

struct Payload
{
    uint flags;
    float3 pathWeight;

    float3 pathColor;
    uint materialIdx;

    RandomNumberGenerator rng;
    uint pad0;
    uint pad1;
    uint pad2;

    HitInfo hitInfo;
};
