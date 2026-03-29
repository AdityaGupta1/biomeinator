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

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "global_params.hlsli"
#include "radiance_cache.hlsli"

RWByteAddressBuffer rcHashEntries : REGISTER_U(RC, HASH_ENTRIES);
RWByteAddressBuffer rcAccumulation : REGISTER_U(RC, ACCUMULATION);
RWStructuredBuffer<float4> rcResolved : REGISTER_U(RC, RESOLVED);

[shader("compute")]
[numthreads(RC_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadId.x;
    if (slot >= RC_TABLE_SIZE)
    {
        return;
    }

    // clear accumulation buffer
    rcAccumulation.Store4(slot * 16, uint4(0, 0, 0, 0));

    // evict resolved entries that have fallen below the stale weight threshold
    const uint2 key = rcHashEntries.Load2(slot * 8);
    if (any(key != RC_EMPTY_SENTINEL))
    {
        if (rcResolved[slot].w < RC_STALE_WEIGHT_THRESHOLD)
        {
            rcHashEntries.Store2(slot * 8, uint2(RC_EMPTY_SENTINEL, RC_EMPTY_SENTINEL));
            rcResolved[slot] = float4(0, 0, 0, 0);
        }
    }
}
