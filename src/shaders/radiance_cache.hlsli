/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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

#include "../rendering/common/common_settings.h"

#include "util/rng.hlsli"

#define RC_STALE_WEIGHT_THRESHOLD 0.1f
#define RC_RADIANCE_SCALE 128.f
#define RC_SAMPLE_MULTIPLIER 1024
#define RC_DECAY 0.95f
#define RC_JITTER_SCALE 0.2f
#define RC_MIN_LEVEL (-4)
#define RC_MAX_LEVEL 11
#define RC_LEVEL_OFFSET 4 // = -RC_MIN_LEVEL, maps level to unsigned for packing
#define RC_GRID_OFFSET 0.37f

int rcGetLevel(float3 pos_WS)
{
    const float dist = length(pos_WS - cameraParams.pos_WS);
    return clamp(int(floor(log2(dist * rcParams.rcCascadeScale))), RC_MIN_LEVEL, RC_MAX_LEVEL);
}

float rcGetVoxelSize(int level)
{
    return exp2(float(level));
}

int3 rcWorldToGrid(float3 pos_WS, int level)
{
    const float voxelSize = rcGetVoxelSize(level);
    const int3 offset = cameraParams.globalInstanceOffset;

    if (level > 0)
    {
        const int3 offsetGrid = offset >> level;
        const float3 offsetFrac = float3(offset & ((1u << level) - 1)) / voxelSize;
        return int3(floor(pos_WS / voxelSize + offsetFrac + RC_GRID_OFFSET)) + offsetGrid;
    }
    else
    {
        const int3 offsetGrid = offset << (-level);
        return int3(floor(pos_WS / voxelSize + RC_GRID_OFFSET)) + offsetGrid;
    }
}

uint rcSpatialHash(int3 gridPos, int level)
{
    uint h = (uint)gridPos.x * 73856093u
           ^ (uint)gridPos.y * 19349663u
           ^ (uint)gridPos.z * 83492791u
           ^ (uint)(level + RC_LEVEL_OFFSET) * 2654435761u;
    h = (h ^ 61u) ^ (h >> 16u);
    h *= 9u;
    h ^= h >> 4u;
    h *= 0x27d4eb2du;
    h ^= h >> 15u;
    return h & (RC_TABLE_SIZE - 1u);
}

// Key bit layout:
//   key.x: [0..19]  x (20 bits), [20..31] y low (12 bits)
//   key.y: [0..3]   y high (4 bits), [4..23] z (20 bits), [24..27] level (4 bits), [28..31] unused (zeroed)
uint2 rcPackKey(int3 gridPos, int level)
{
    uint2 key;
    key.x = (uint(gridPos.x) & 0xFFFFF) | ((uint(gridPos.y) & 0xFFF) << 20);
    key.y = ((uint(gridPos.y) >> 12) & 0xF)
          | ((uint(gridPos.z) & 0xFFFFF) << 4)
          | ((uint(level + RC_LEVEL_OFFSET) & 0xF) << 24);
    return key;
}

uint rcInsertOrFind(int3 gridPos, int level, RWByteAddressBuffer hashEntries)
{
    uint slot = rcSpatialHash(gridPos, level);
    const uint2 key = rcPackKey(gridPos, level);

    for (uint probe = 0; probe < 8; ++probe)
    {
        uint originalX;
        hashEntries.InterlockedCompareExchange(slot * 8, RC_EMPTY_SENTINEL, key.x, originalX);

        if (originalX == RC_EMPTY_SENTINEL)
        {
            hashEntries.Store(slot * 8 + 4, key.y);
            return slot;
        }

        if (originalX == key.x)
        {
            const uint storedY = hashEntries.Load(slot * 8 + 4);
            if (storedY == key.y)
            {
                return slot;
            }
        }

        slot = (slot + 1) & (RC_TABLE_SIZE - 1u);
    }

    return ~0u;
}

uint rcLookup(int3 gridPos, int level, ByteAddressBuffer hashEntries)
{
    uint slot = rcSpatialHash(gridPos, level);
    const uint2 key = rcPackKey(gridPos, level);

    for (uint probe = 0; probe < 8; ++probe)
    {
        const uint2 stored = hashEntries.Load2(slot * 8);

        if (stored.x == key.x && stored.y == key.y)
        {
            return slot;
        }

        slot = (slot + 1) & (RC_TABLE_SIZE - 1u);
    }

    return ~0u;
}

float3 rcJitterPos(float3 pos_WS, int level, inout RandomNumberGenerator rng)
{
    const float3 jitter = (rng.nextFloat3() - 0.5f) * 2.f * RC_JITTER_SCALE;
    return pos_WS + jitter * rcGetVoxelSize(level);
}

void rcWriteRadiance(uint slot, float3 radiance, RWByteAddressBuffer accumBuffer)
{
    const uint3 quantized = uint3(max(radiance, 0) * RC_RADIANCE_SCALE);
    accumBuffer.InterlockedAdd(slot * 16 + 0, quantized.x);
    accumBuffer.InterlockedAdd(slot * 16 + 4, quantized.y);
    accumBuffer.InterlockedAdd(slot * 16 + 8, quantized.z);
}

void rcWriteSampleCount(uint slot, RWByteAddressBuffer accumBuffer)
{
    accumBuffer.InterlockedAdd(slot * 16 + 12, RC_SAMPLE_MULTIPLIER);
}
