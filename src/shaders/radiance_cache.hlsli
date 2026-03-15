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

int3 rcWorldToGrid(float3 pos_WS, float voxelSize)
{
    return int3(floor(pos_WS / voxelSize));
}

uint rcSpatialHash(int3 gridPos)
{
    uint h = (uint)gridPos.x * 73856093u
           ^ (uint)gridPos.y * 19349663u
           ^ (uint)gridPos.z * 83492791u;
    h = (h ^ 61u) ^ (h >> 16u);
    h *= 9u;
    h ^= h >> 4u;
    h *= 0x27d4eb2du;
    h ^= h >> 15u;
    return h & (RC_TABLE_SIZE - 1u);
}

uint2 rcPackKey(int3 gridPos)
{
    uint2 key;
    key.x = (uint(gridPos.x) & 0xFFFFFF) | ((uint(gridPos.y) & 0xFF) << 24);
    key.y = ((uint(gridPos.y) >> 8) & 0xFF) | ((uint(gridPos.z) & 0xFFFFFF) << 8);
    return key;
}

uint rcInsertOrFind(int3 gridPos, RWByteAddressBuffer hashEntries)
{
    uint slot = rcSpatialHash(gridPos);
    const uint2 key = rcPackKey(gridPos);

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

uint rcLookup(int3 gridPos, ByteAddressBuffer hashEntries)
{
    uint slot = rcSpatialHash(gridPos);
    const uint2 key = rcPackKey(gridPos);

    for (uint probe = 0; probe < 8; ++probe)
    {
        const uint2 stored = hashEntries.Load2(slot * 8);

        if (stored.x == RC_EMPTY_SENTINEL && stored.y == RC_EMPTY_SENTINEL)
        {
            return ~0u;
        }

        if (stored.x == key.x && stored.y == key.y)
        {
            return slot;
        }

        slot = (slot + 1) & (RC_TABLE_SIZE - 1u);
    }

    return ~0u;
}

float3 rcJitterPos(float3 pos_WS, float voxelSize, inout RandomNumberGenerator rng)
{
    return pos_WS + (rng.nextFloat3() - 0.5f) * RC_JITTER_SCALE * voxelSize;
}

void rcWriteRadiance(uint slot, float3 radiance, RWByteAddressBuffer accumBuffer)
{
    const uint3 quantized = uint3(max(radiance, 0) * RC_RADIANCE_SCALE);
    accumBuffer.InterlockedAdd(slot * 16 + 0, quantized.x);
    accumBuffer.InterlockedAdd(slot * 16 + 4, quantized.y);
    accumBuffer.InterlockedAdd(slot * 16 + 8, quantized.z);
    accumBuffer.InterlockedAdd(slot * 16 + 12, RC_SAMPLE_MULTIPLIER);
}
