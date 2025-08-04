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

#include "util/rng.slang"

#define PAYLOAD_FLAG_PATH_FINISHED (1 << 0)

struct HitInfo
{
    float3 normal_WS;
    float hitT;

    float2 uv;
    uint instanceId;
    uint triangleIdx;
};

struct Payload
{
    float3 pathWeight;
    uint flags;

    float3 pathColor;
    uint materialId;

    HitInfo hitInfo;

    float lastBsdfPdf;
    uint lastSampleWasSpecular;

    RandomSampler rng;
};
